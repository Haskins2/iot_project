/**
 * @file water_sensor.c
 * @brief Water level sensor implementation
 */

#include "water_sensor.h"

#include "esp_log.h"

/* ------- Pin / ADC configuration (unchanged from original repo) ------- */
#define WATER_SENSOR_ADC_CHANNEL  ADC_CHANNEL_4   /* GPIO 4 on ESP32-C6 */
#define WATER_SENSOR_ADC_ATTEN    ADC_ATTEN_DB_12 /* Full-scale ~3.3 V  */
#define WATER_DETECTION_THRESHOLD 300

static const char *TAG = "water_sensor";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;

/* -------------------------------------------------------------------- */

esp_err_t WaterSensorInit(adc_oneshot_unit_handle_t adc_handle)
{
    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "ADC handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    s_adc_handle = adc_handle;

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = WATER_SENSOR_ADC_ATTEN,
    };
    esp_err_t err = adc_oneshot_config_channel(s_adc_handle, WATER_SENSOR_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Water sensor ADC channel configured (ADC1 CH%d)", WATER_SENSOR_ADC_CHANNEL);

    return ESP_OK;
}

esp_err_t RetrieveWaterSensorData(water_data_t *data)
{
    if (data == NULL || s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = adc_oneshot_read(s_adc_handle, WATER_SENSOR_ADC_CHANNEL, &data->raw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

bool IsWaterDetected(const water_data_t *data)
{
    return (data != NULL) && (data->raw > WATER_DETECTION_THRESHOLD);
}
