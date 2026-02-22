/**
 * Water Sensing with Servo Actuation
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"

/* ------- Configuration ------- */

// Water Sensor (ADC)
#define WATER_SENSOR_ADC_CHANNEL ADC_CHANNEL_4
#define WATER_SENSOR_ADC_UNIT ADC_UNIT_1
#define WATER_SENSOR_ADC_ATTEN ADC_ATTEN_DB_12 // Full-scale ~3.3V
#define WATER_DETECTION_THRESHOLD 300          // ADC threshold for water detection

// Servo Motor (PWM via LEDC)
#define SERVO_PIN 21
#define SERVO_CHANNEL LEDC_CHANNEL_0
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_FREQ 50 // Standard servo frequency (Hz)
#define SERVO_RESOLUTION LEDC_TIMER_16_BIT

// Servo pulse width limits (microseconds) - adjust for your servo model
#define SERVO_MIN_PULSE_US 500
#define SERVO_MAX_PULSE_US 2500

// Timing
#define SENSOR_READ_INTERVAL_MS 1000

static const char *TAG = "water_sensor";

/* ------- Servo Control ------- */

static void set_servo_angle(int angle)
{
    // Clamp angle to valid range
    if (angle < 0)
        angle = 0;
    if (angle > 180)
        angle = 180;

    // Convert pulse width (µs) to duty cycle ticks (from datasheet pdf)
    // Formula: ticks = (pulse_us / period_us) * max_ticks
    // At 50Hz: period = 20000µs, max_ticks = 65536 (16-bit)
    uint32_t min_duty = (SERVO_MIN_PULSE_US * 65536) / 20000; // ~1638
    uint32_t max_duty = (SERVO_MAX_PULSE_US * 65536) / 20000; // ~8192
    uint32_t duty = min_duty + ((angle * (max_duty - min_duty)) / 180);

    ESP_LOGI(TAG, "Servo: angle=%d°, duty=%lu", angle, duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL);
}

void app_main(void)
{
    // ADC INIT
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = WATER_SENSOR_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = WATER_SENSOR_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, WATER_SENSOR_ADC_CHANNEL, &config));

    // LEDC INIT
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_RESOLUTION,
        .timer_num = SERVO_TIMER,
        .freq_hz = SERVO_FREQ,
        .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t channel_conf = {
        .gpio_num = SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = SERVO_CHANNEL,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
        .hpoint = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));

    while (1)
    {
        // ADC READ
        int adc_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, WATER_SENSOR_ADC_CHANNEL, &adc_raw));
        ESP_LOGI(TAG, "Water Sensor Raw Value: %d", adc_raw);

        // ACTUATE SERVO
        if (adc_raw > 300)
        {
            ESP_LOGI(TAG, "Water detected! Opening servo.");
            set_servo_angle(90);
        }
        else
        {
            set_servo_angle(0);
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
