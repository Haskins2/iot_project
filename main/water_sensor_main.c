#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"

// ADC Channel for Water Sensor (GPIO 4 on ESP32-C6 is ADC1 Channel 4)
#define WATER_SENSOR_ADC_CHANNEL ADC_CHANNEL_4
#define WATER_SENSOR_ADC_UNIT ADC_UNIT_1 /
#define WATER_SENSOR_ADC_ATTEN ADC_ATTEN_DB_12 // sets attenuation to 12db. tune this?

static const char *TAG = "water_sensor";

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

    while (1)
    {
        // ADC READ
        int adc_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, WATER_SENSOR_ADC_CHANNEL, &adc_raw));
        ESP_LOGI(TAG, "Water Sensor Raw Value: %d", adc_raw);

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
