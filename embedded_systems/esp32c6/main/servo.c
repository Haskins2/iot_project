/**
 * @file servo.c
 * @brief DIYMORE-DM996 Servo motor implementation (PWM via LEDC)
 */

#include "servo.h"

#include "driver/ledc.h"
#include "esp_log.h"

/* ------- Configuration (unchanged from original repo) ------- */
#define SERVO_PIN            21
#define SERVO_CHANNEL        LEDC_CHANNEL_0
#define SERVO_TIMER          LEDC_TIMER_0
#define SERVO_FREQ           50              /* Standard servo 50 Hz */
#define SERVO_RESOLUTION     LEDC_TIMER_16_BIT

/* Pulse width limits in microseconds (from MG996R datasheet) */
#define SERVO_MIN_PULSE_US   500
#define SERVO_MAX_PULSE_US   2500

static const char *TAG = "servo";

/* -------------------------------------------------------------------- */

esp_err_t ServoInit(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_RESOLUTION,
        .timer_num       = SERVO_TIMER,
        .freq_hz         = SERVO_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t channel_conf = {
        .gpio_num   = SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = SERVO_CHANNEL,
        .timer_sel  = SERVO_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&channel_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Servo initialised on GPIO %d", SERVO_PIN);
    return ESP_OK;
}

void ActuateServo(int angle)
{
    /* Clamp to valid range */
    if (angle < 0)   angle = 0;
    if (angle > 180) angle = 180;

    /*
     * Convert angle → duty-cycle ticks.
     * At 50 Hz the period is 20 000 µs; with 16-bit resolution
     * max ticks = 65 536.
     *   duty = (pulse_us / 20000) * 65536
     */
    uint32_t min_duty = (SERVO_MIN_PULSE_US * 65536) / 20000;
    uint32_t max_duty = (SERVO_MAX_PULSE_US * 65536) / 20000;
    uint32_t duty     = min_duty + ((angle * (max_duty - min_duty)) / 180);

    ESP_LOGI(TAG, "Servo: angle=%d°, duty=%lu", angle, (unsigned long)duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL);
}
