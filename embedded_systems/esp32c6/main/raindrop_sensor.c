/**
 * @file raindrop_sensor.c
 * @brief FC-37 Raindrop Sensor implementation
 */

#include "raindrop_sensor.h"

#include "driver/gpio.h"
#include "esp_log.h"

/* ------- Pin / ADC configuration (unchanged from original repo) ------- */
#define FC37_DO_PIN          GPIO_NUM_3
#define FC37_A0_ADC_CHANNEL  ADC_CHANNEL_2   /* GPIO 2 on ESP32-C6 */
#define FC37_A0_ADC_ATTEN    ADC_ATTEN_DB_12 /* Full-scale ~3.3 V  */

static const char *TAG = "raindrop_sensor";

/* Stored handle so RetrieveRaindropSensorData can read without extra args */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

/* -------------------------------------------------------------------- */

esp_err_t RaindropSensorInit(adc_oneshot_unit_handle_t adc_handle)
{
    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "ADC handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    s_adc_handle = adc_handle;

    /* 1. Digital output (DO) — input with pull-up */
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << FC37_DO_PIN),
        .pull_down_en = 0,
        .pull_up_en   = 1,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure DO pin: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "DO pin configured (GPIO %d)", FC37_DO_PIN);

    /* 2. Analog output (A0) — configure ADC channel */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = FC37_A0_ADC_ATTEN,
    };
    err = adc_oneshot_config_channel(s_adc_handle, FC37_A0_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "A0 ADC channel configured (ADC1 CH%d)", FC37_A0_ADC_CHANNEL);

    return ESP_OK;
}

esp_err_t RetrieveRaindropSensorData(raindrop_data_t *data)
{
    if (data == NULL || s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    data->digital = gpio_get_level(FC37_DO_PIN);

    esp_err_t err = adc_oneshot_read(s_adc_handle, FC37_A0_ADC_CHANNEL, &data->analog);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

bool IsRainDetected(const raindrop_data_t *data)
{
    /* DO pin is active-low: 0 = rain detected */
    return (data != NULL) && (data->digital == 0);
}
