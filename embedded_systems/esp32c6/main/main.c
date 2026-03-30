/**
 * @file main.c
 * @brief Flood Detection System — ESP32-C6 master controller
 *
 * Combined firmware merging:
 *   - Embedded systems: sensors, servo, pump, BLE camera (combined_hardware)
 *   - Device-to-cloud:  WiFi, MQTT (mTLS), OTA updates (main)
 *
 * Subsystems:
 *   1. FC-37 raindrop sensor  (digital + analog)
 *   2. Water level sensor     (analog)
 *   3. Servo actuation        (PWM)
 *   4. Water pump             (digital GPIO)
 *   5. ESP-EYE BLE camera     (GATT client)
 *   6. WiFi + MQTT            (mTLS, NVS-stored certs)
 *   7. OTA firmware updates   (via GitHub releases)
 *
 * The main loop always runs (sensors + local actuation) regardless of
 * WiFi/MQTT connectivity.  Cloud features are used when available.
 *
 * DEBUG_MODE:  When true, local flood detection actuates servo/pump/camera
 *              based on sensor readings.  Cloud actuation works independently
 *              via MQTT commands on devices/{id}/actuate.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "cJSON.h"
#include "ota.h"

/* Embedded subsystem headers */
#include "raindrop_sensor.h"
#include "water_sensor.h"
#include "servo.h"
#include "pump.h"
#include "ble_client.h"
#include "sensor_packet.h"
#include "image_reassembly.h"

/* ===================================================================== */
/*  DEBUG flag — enables local flood-detection actuation for testing      */
/* ===================================================================== */
#define DEBUG_MODE  true

static const char *TAG = "flood_main";

/* ===================================================================== */
/*  WiFi credentials (overridden by credentials.h if present)            */
/* ===================================================================== */
#if defined(__has_include)
#  if __has_include("credentials.h")
#    include "credentials.h"
#  else
#    warning "credentials.h not found; using defaults. Create main/credentials.h from credentials.h.example"
#  endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID  "WIFI_SSID"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS  "WIFI_PASS"
#endif
#ifndef MQTT_URI
#define MQTT_URI   "address"
#endif

/* ===================================================================== */
/*  TLS certificate buffers (populated from NVS at runtime)              */
/* ===================================================================== */
static char ca_crt_buf[4096];
static char client_crt_buf[4096];
static char client_key_buf[4096];
static char device_id_buf[64];

/* ===================================================================== */
/*  MQTT topics — built at runtime from device_id                        */
/*                                                                       */
/*  NOTE: Variable names kept for compatibility with existing codebase.  */
/*  MQTT_TOPIC_WATER_DATA -> actually carries the periodic sensor JSON   */
/*  MQTT_TOPIC_RAIN_DATA  -> actually carries the image+sensor JSON      */
/* ===================================================================== */
static char MQTT_TOPIC_WATER_DATA[128];  /* -> devices/{id}/sensor_data  */
static char MQTT_TOPIC_RAIN_DATA[128];   /* -> devices/{id}/image_data   */
static char MQTT_TOPIC_ACTUATION[128];   /* -> devices/{id}/actuate      */

/* ===================================================================== */
/*  Global state                                                         */
/* ===================================================================== */
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* JSON buffer for sensor packets */
#define JSON_BUF_SIZE 256

#define DEFAULT_POLL_INTERVAL_MS  1000

/* Sensor polling interval — modifiable at runtime via SetPollIntervalMs() */
static uint32_t s_poll_interval_ms = DEFAULT_POLL_INTERVAL_MS;

/* Latest sensor readings — shared with BLE image callback via extern */
water_data_t    g_latest_water = { 0 };
raindrop_data_t g_latest_rain  = { 0 };

/* ===================================================================== */
/*  Forward declarations                                                 */
/* ===================================================================== */
static bool wifi_init_sta(void);
static void mqtt_app_start(void);
static void handle_actuation(const char *data, int data_len);
void SetPollIntervalMs(uint32_t interval_ms);
uint32_t GetPollIntervalMs(void);

/* ===================================================================== */
/*  NVS certificate loading                                              */
/* ===================================================================== */

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static esp_err_t load_certs_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("certs", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace 'certs': %s", esp_err_to_name(err));
        return err;
    }

    size_t ca_len   = sizeof(ca_crt_buf);
    size_t crt_len  = sizeof(client_crt_buf);
    size_t key_len  = sizeof(client_key_buf);
    size_t id_len   = sizeof(device_id_buf);

    err = nvs_get_blob(handle, "ca_crt", ca_crt_buf, &ca_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Missing key: ca_crt -> %s", esp_err_to_name(err)); goto fail; }

    err = nvs_get_blob(handle, "client_crt", client_crt_buf, &crt_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Missing key: client_crt -> %s", esp_err_to_name(err)); goto fail; }

    err = nvs_get_blob(handle, "client_key", client_key_buf, &key_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Missing key: client_key -> %s", esp_err_to_name(err)); goto fail; }

    err = nvs_get_str(handle, "device_id", device_id_buf, &id_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Missing key: device_id -> %s", esp_err_to_name(err)); goto fail; }

    ESP_LOGI(TAG, "Certs loaded from NVS (ca=%d, crt=%d, key=%d bytes)",
             (int)ca_len, (int)crt_len, (int)key_len);
    ESP_LOGI(TAG, "Device ID: %s", device_id_buf);
    nvs_close(handle);

    /* Build MQTT topic strings from device ID
     * MQTT_TOPIC_WATER_DATA = periodic sensor data (named for legacy compat) */
    snprintf(MQTT_TOPIC_WATER_DATA, sizeof(MQTT_TOPIC_WATER_DATA),
             "devices/%s/sensor_data", device_id_buf);
    /* MQTT_TOPIC_RAIN_DATA = image + sensor data (named for legacy compat) */
    snprintf(MQTT_TOPIC_RAIN_DATA, sizeof(MQTT_TOPIC_RAIN_DATA),
             "devices/%s/image_data", device_id_buf);
    snprintf(MQTT_TOPIC_ACTUATION, sizeof(MQTT_TOPIC_ACTUATION),
             "devices/%s/actuate", device_id_buf);

    ESP_LOGI(TAG, "Topics: sensor=%s, image=%s, actuate=%s",
             MQTT_TOPIC_WATER_DATA, MQTT_TOPIC_RAIN_DATA, MQTT_TOPIC_ACTUATION);

    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Failed to load cert from NVS: %s", esp_err_to_name(err));
    nvs_close(handle);
    return err;
}

/* ===================================================================== */
/*  WiFi                                                                 */
/* ===================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi: Starting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi: Disconnected! Reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi: Connected! IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * Initialise WiFi in STA mode.
 * Returns true if connected within 30 s, false otherwise.
 * WiFi keeps trying to reconnect in the background even if this returns false.
 */
static bool wifi_init_sta(void)
{
    ESP_LOGI(TAG, "Wi-Fi: Initialising with SSID '%s'", WIFI_SSID);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi: Waiting for connection (30 s timeout)...");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi: Successfully connected!");
        return true;
    } else {
        ESP_LOGW(TAG, "Wi-Fi: Connection timeout — continuing without cloud");
        return false;
    }
}

/* ===================================================================== */
/*  MQTT                                                                 */
/* ===================================================================== */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        mqtt_connected = true;

        /* Subscribe to actuation commands from the cloud */
        esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_ACTUATION, 1);
        ESP_LOGI(TAG, "MQTT: Subscribed to %s", MQTT_TOPIC_ACTUATION);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA: {
        char topic[128] = {0};
        char data[512]  = {0};
        snprintf(topic, sizeof(topic), "%.*s", event->topic_len, event->topic);
        snprintf(data,  sizeof(data),  "%.*s", event->data_len,  event->data);

        ESP_LOGI(TAG, "MQTT: Received [%s] -> %s", topic, data);

        if (strncmp(topic, MQTT_TOPIC_ACTUATION, event->topic_len) == 0) {
            handle_actuation(data, event->data_len);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls",
                                 event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack",
                                 event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",
                                 event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)",
                     strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;

    default:
        ESP_LOGI(TAG, "MQTT: Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    if (load_certs_from_nvs() != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start MQTT without certificates");
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_URI,
            .verification = {
                .certificate     = ca_crt_buf,
                .certificate_len = strlen(ca_crt_buf) + 1,
            },
        },
        .credentials = {
            .client_id = device_id_buf,
            .authentication = {
                .certificate     = client_crt_buf,
                .certificate_len = strlen(client_crt_buf) + 1,
                .key             = client_key_buf,
                .key_len         = strlen(client_key_buf) + 1,
            },
        },
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT: Failed to initialise client");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
    ESP_LOGI(TAG, "MQTT: Connecting to %s (client: %s)", MQTT_URI, device_id_buf);
}

/* ===================================================================== */
/*  MQTT publish helpers (called from main loop and ble_client.c)        */
/* ===================================================================== */

/**
 * Publish an image+sensor JSON payload to the image_data topic.
 * Called from ble_client.c when image reassembly completes.
 * esp_mqtt_client_publish() is thread-safe — safe from BLE callback context.
 */
void mqtt_publish_image(const char *payload, int len)
{
    if (mqtt_client && mqtt_connected) {
        int msg_id = esp_mqtt_client_publish(
            mqtt_client, MQTT_TOPIC_RAIN_DATA, payload, len, 1, 0);
        ESP_LOGI(TAG, "MQTT: Published image_data msg_id=%d (%d bytes)", msg_id, len);
    } else {
        ESP_LOGW(TAG, "MQTT: Not connected — image payload not published");
    }
}

/* ===================================================================== */
/*  Cloud actuation handler                                              */
/* ===================================================================== */

/**
 * Parse and execute actuation commands received from the cloud.
 *
 * Expected JSON format:
 *   {
 *     "servo": 90,              // angle 0-180 (optional)
 *     "pump": 1,                // 0=off, 1=on  (optional)
 *     "capture_image": true,    // trigger camera (optional)
 *     "poll_interval": 500      // ms (optional)
 *   }
 */
static void handle_actuation(const char *data, int data_len)
{
    ESP_LOGI(TAG, "MQTT: Actuation received: %.*s", data_len, data);

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root) {
        ESP_LOGE(TAG, "MQTT: Failed to parse actuation JSON");
        return;
    }

    cJSON *servo = cJSON_GetObjectItem(root, "servo");
    if (cJSON_IsNumber(servo)) {
        int angle = servo->valueint;
        ESP_LOGI(TAG, "MQTT: Actuating servo to %d degrees", angle);
        ActuateServo(angle);
    }

    cJSON *pump = cJSON_GetObjectItem(root, "pump");
    if (cJSON_IsNumber(pump)) {
        if (pump->valueint) {
            ESP_LOGI(TAG, "MQTT: Pump ON");
            PumpOn();
        } else {
            ESP_LOGI(TAG, "MQTT: Pump OFF");
            PumpOff();
        }
    }

    cJSON *capture = cJSON_GetObjectItem(root, "capture_image");
    if (cJSON_IsTrue(capture)) {
        ESP_LOGI(TAG, "MQTT: Triggering camera capture");
        TriggerCameraCapture();
    }

    cJSON *poll = cJSON_GetObjectItem(root, "poll_interval");
    if (cJSON_IsNumber(poll)) {
        ESP_LOGI(TAG, "MQTT: Setting poll interval to %d ms", poll->valueint);
        SetPollIntervalMs((uint32_t)poll->valueint);
    }

    cJSON_Delete(root);
}

/* ===================================================================== */
/*  app_main                                                             */
/* ===================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Flood Detection System — Starting Up");
    ESP_LOGI(TAG, "  DEBUG_MODE = %s", DEBUG_MODE ? "ON" : "OFF");
    ESP_LOGI(TAG, "========================================");

    /* ── 1. NVS flash (required by WiFi, BLE, and MQTT cert storage) ── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS flash initialised");

    /* ── 2. WiFi (non-blocking — system continues even if WiFi fails) ─ */
    bool wifi_ok = wifi_init_sta();

    /* ── 3. OTA + MQTT (only if WiFi connected) ───────────────────── */
    if (wifi_ok) {
        /* Mark current firmware as valid (prevents OTA rollback) */
        ota_mark_valid();

        /* Start OTA task — checks immediately then every 24 hrs */
        xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);

        /* Give OTA time to complete version check before starting MQTT */
        vTaskDelay(pdMS_TO_TICKS(1000000));

        /* Start MQTT (loads certs from NVS, connects to broker) */
        mqtt_app_start();
    } else {
        ESP_LOGW(TAG, "Skipping OTA and MQTT — no WiFi connection");
    }

    /* ── 4. Shared ADC1 handle ─────────────────────────────────────── */
    /*    Both the water sensor (CH4) and raindrop analog (CH2)        */
    /*    live on ADC_UNIT_1, so we create one handle and share it.    */
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t adc_init = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init, &adc1_handle));
    ESP_LOGI(TAG, "ADC1 unit initialised (shared handle)");

    /* ── 5. Initialise embedded subsystems ─────────────────────────── */
    ESP_ERROR_CHECK(RaindropSensorInit(adc1_handle));
    ESP_ERROR_CHECK(WaterSensorInit(adc1_handle));
    ESP_ERROR_CHECK(ServoInit());
    ESP_ERROR_CHECK(PumpInit());
    ESP_ERROR_CHECK(BleClientInit());          /* non-blocking — scans in background */
    ESP_ERROR_CHECK(image_reassembly_init());

    ESP_LOGI(TAG, "All subsystems initialised — entering sensor loop");

    /* ── 6. Sensor polling loop ────────────────────────────────────── */
    char json_buf[JSON_BUF_SIZE];
    bool prev_flood_state = false;

    while (1) {
        /* ---- Read sensors ---- */
        water_data_t    water = { 0 };
        raindrop_data_t rain  = { 0 };

        esp_err_t w_err = RetrieveWaterSensorData(&water);
        esp_err_t r_err = RetrieveRaindropSensorData(&rain);

        /* Update shared globals for BLE image callback */
        g_latest_water = water;
        g_latest_rain  = rain;

        if (w_err != ESP_OK) {
            ESP_LOGE(TAG, "Water sensor read error: %s", esp_err_to_name(w_err));
        }
        if (r_err != ESP_OK) {
            ESP_LOGE(TAG, "Raindrop sensor read error: %s", esp_err_to_name(r_err));
        }

        /* ---- Log sensor readings ---- */
        bool ble_ok = IsBleConnected();

        ESP_LOGI(TAG, "Water: raw=%d (%s) | Rain: DO=%d (%s), A0=%d | BLE: %s",
                 water.raw,
                 IsWaterDetected(&water) ? "WATER" : "DRY",
                 rain.digital,
                 IsRainDetected(&rain)   ? "RAIN"  : "DRY",
                 rain.analog,
                 ble_ok ? "CONNECTED" : "DISCONNECTED");

        /* ---- Format JSON sensor packet ---- */
        int n = FormatSensorPacket(&water, &rain, ble_ok,
                                   json_buf, JSON_BUF_SIZE);
        if (n > 0) {
            ESP_LOGI(TAG, "JSON: %s", json_buf);
        }

        /* ---- Publish sensor data to MQTT (if connected) ---- */
        if (mqtt_client && mqtt_connected && n > 0) {
            int msg_id = esp_mqtt_client_publish(
                mqtt_client, MQTT_TOPIC_WATER_DATA, json_buf, 0, 1, 0);
            ESP_LOGI(TAG, "MQTT: Published sensor_data msg_id=%d", msg_id);
        }

        /* ---- Local flood detection + actuation ---- */
        if (DEBUG_MODE) {
            bool flood_now = IsWaterDetected(&water) && IsRainDetected(&rain);

            if (flood_now && !prev_flood_state) {
                /* Rising edge — both sensors just triggered */
                ESP_LOGW(TAG, "[DEBUG] Flood condition detected — actuating!");
                ActuateServo(90);
                PumpOn();
                TriggerCameraCapture();   /* safe even if BLE is down */
            } else if (!flood_now && prev_flood_state) {
                /* Falling edge — condition cleared */
                ESP_LOGI(TAG, "[DEBUG] Flood condition cleared — resetting actuators");
                ActuateServo(0);
                PumpOff();
            }

            prev_flood_state = flood_now;
        }

        /* ---- Wait for next poll ---- */
        vTaskDelay(pdMS_TO_TICKS(s_poll_interval_ms));
    }
}

/* ===================================================================== */
/*  Cloud-facing API                                                      */
/* ===================================================================== */

void SetPollIntervalMs(uint32_t interval_ms)
{
    if (interval_ms < 100) {
        ESP_LOGW(TAG, "Poll interval clamped to 100 ms (requested %" PRIu32 ")",
                 interval_ms);
        interval_ms = 100;
    }
    s_poll_interval_ms = interval_ms;
    ESP_LOGI(TAG, "Poll interval set to %" PRIu32 " ms", s_poll_interval_ms);
}

uint32_t GetPollIntervalMs(void)
{
    return s_poll_interval_ms;
}
