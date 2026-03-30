/**
 * @file pump.c
 * @brief Water pump implementation (digital on/off via GPIO 18)
 */

#include "pump.h"

#include "driver/gpio.h"
#include "esp_log.h"

#define PUMP_GPIO  GPIO_NUM_18

static const char *TAG = "pump";

esp_err_t PumpInit(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PUMP_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_set_level(PUMP_GPIO, 0);
    ESP_LOGI(TAG, "Pump initialised on GPIO %d (OFF)", PUMP_GPIO);
    return ESP_OK;
}

void PumpOn(void)
{
    gpio_set_level(PUMP_GPIO, 1);
    ESP_LOGI(TAG, "Pump ON");
}

void PumpOff(void)
{
    gpio_set_level(PUMP_GPIO, 0);
    ESP_LOGI(TAG, "Pump OFF");
}
