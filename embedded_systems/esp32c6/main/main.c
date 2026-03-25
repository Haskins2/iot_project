/**
 * @file main.c
 * @brief Flood Detection System — ESP32-C6 master controller
 *
 * Combines:
 *   1. FC-37 raindrop sensor  (digital + analog)
 *   2. Water level sensor     (analog)
 *   3. Servo actuation        (PWM)
 *   4. ESP-EYE BLE camera     (GATT client)
 *
 * All sensor data is printed to console at 1 Hz and formatted into a
 * JSON packet ready for cloud transmission.
 *
 * DEBUG mode:  When DEBUG_MODE is true and BOTH sensors detect water,
 *              the servo is actuated to 90° and a camera capture is
 *              triggered on the ESP-EYE.
 */

#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"

#include "raindrop_sensor.h"
#include "water_sensor.h"
#include "servo.h"
#include "ble_client.h"
#include "sensor_packet.h"

/* ===================================================================== */
/*  DEBUG flag — set to true to enable local actuation logic for testing  */
/* ===================================================================== */
#define DEBUG_MODE  true

static const char *TAG = "flood_main";

/* JSON buffer — comfortably sized for our packet */
#define JSON_BUF_SIZE 256

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Flood Detection System — Starting Up");
    ESP_LOGI(TAG, "  DEBUG_MODE = %s", DEBUG_MODE ? "ON" : "OFF");
    ESP_LOGI(TAG, "========================================");

    /* ── 1. Shared ADC1 handle ─────────────────────────────────────── */
    /*    Both the water sensor (CH4) and raindrop analog (CH2)        */
    /*    live on ADC_UNIT_1, so we create one handle and share it.    */
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t adc_init = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init, &adc1_handle));
    ESP_LOGI(TAG, "ADC1 unit initialised (shared handle)");

    /* ── 2. Initialise subsystems ──────────────────────────────────── */
    ESP_ERROR_CHECK(RaindropSensorInit(adc1_handle));
    ESP_ERROR_CHECK(WaterSensorInit(adc1_handle));
    ESP_ERROR_CHECK(ServoInit());
    ESP_ERROR_CHECK(BleClientInit());   /* non-blocking — scans in background */

    ESP_LOGI(TAG, "All subsystems initialised — entering sensor loop");

    /* ── 3. Sensor polling loop (1 Hz) ─────────────────────────────── */
    char json_buf[JSON_BUF_SIZE];
    bool prev_flood_state = false;  /* track state changes for actuation */

    while (1) {
        /* ---- Read sensors ---- */
        water_data_t    water = { 0 };
        raindrop_data_t rain  = { 0 };

        esp_err_t w_err = RetrieveWaterSensorData(&water);
        esp_err_t r_err = RetrieveRaindropSensorData(&rain);

        if (w_err != ESP_OK) {
            ESP_LOGE(TAG, "Water sensor read error: %s", esp_err_to_name(w_err));
        }
        if (r_err != ESP_OK) {
            ESP_LOGE(TAG, "Raindrop sensor read error: %s", esp_err_to_name(r_err));
        }

        /* ---- Print individual sensor readings ---- */
        bool ble_ok = IsBleConnected();

        ESP_LOGI(TAG, "Water: raw=%d (%s) | Rain: DO=%d (%s), A0=%d | BLE: %s",
                 water.raw,
                 IsWaterDetected(&water) ? "WATER" : "DRY",
                 rain.digital,
                 IsRainDetected(&rain)   ? "RAIN"  : "DRY",
                 rain.analog,
                 ble_ok ? "CONNECTED" : "DISCONNECTED");

        /* ---- Format JSON packet for cloud ---- */
        int n = FormatSensorPacket(&water, &rain, ble_ok,
                                   json_buf, JSON_BUF_SIZE);
        if (n > 0) {
            ESP_LOGI(TAG, "JSON: %s", json_buf);
        }

        /* ---- DEBUG mode: local actuation ---- */
        if (DEBUG_MODE) {
            bool flood_now = IsWaterDetected(&water) && IsRainDetected(&rain);

            if (flood_now && !prev_flood_state) {
                /* Rising edge — both sensors just triggered */
                ESP_LOGW(TAG, "[DEBUG] Flood condition detected — actuating!");
                ActuateServo(90);
                TriggerCameraCapture();   /* safe even if BLE is down */
            } else if (!flood_now && prev_flood_state) {
                /* Falling edge — condition cleared */
                ESP_LOGI(TAG, "[DEBUG] Flood condition cleared — resetting servo");
                ActuateServo(0);
            }

            prev_flood_state = flood_now;
        }

        /* ---- Wait 1 second ---- */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
