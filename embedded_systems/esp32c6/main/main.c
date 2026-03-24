/**
 * FC-37 Raindrop Sensor Test
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

/* ------- Configuration ------- */

// FC-37 Digital Out (DO)
#define FC37_DO_PIN GPIO_NUM_3

// FC-37 Analog Out (A0)
#define FC37_A0_ADC_CHANNEL ADC_CHANNEL_2 // GPIO 2 is ADC1 Channel 2
#define FC37_A0_ADC_UNIT ADC_UNIT_1
#define FC37_A0_ADC_ATTEN ADC_ATTEN_DB_12 // Full-scale ~3.3V

static const char *TAG = "raindrop_sensor";

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing FC-37 Raindrop Sensor Test...");

    /* 1. Configure DO (Digital Input) */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << FC37_DO_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1 // Enable pull-up just in case
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Configured GPIO %d for DO (Digital Input)", FC37_DO_PIN);

    /* 2. Configure A0 (Analog Input) */
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = FC37_A0_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = FC37_A0_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, FC37_A0_ADC_CHANNEL, &config));
    ESP_LOGI(TAG, "Configured ADC Unit %d, Channel %d for A0 (Analog Input)", FC37_A0_ADC_UNIT, FC37_A0_ADC_CHANNEL);

    ESP_LOGI(TAG, "Starting sensor read loop...");

    /* 3. Polling Loop */
    while (1)
    {
        // Read DO (Digital)
        int do_state = gpio_get_level(FC37_DO_PIN);

        // Read A0 (Analog)
        int a0_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, FC37_A0_ADC_CHANNEL, &a0_raw));

        // Log results
        ESP_LOGI(TAG, "FC-37 | DO: %d (%s) | A0: %d", 
                 do_state, 
                 (do_state == 0) ? "RAIN DETECTED" : "DRY",
                 a0_raw);

        // Wait 1 second
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
