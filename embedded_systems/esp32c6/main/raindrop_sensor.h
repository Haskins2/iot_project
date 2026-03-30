/**
 * @file raindrop_sensor.h
 * @brief FC-37 Raindrop Sensor interface
 *
 * Digital output (DO): GPIO 3
 * Analog output (A0): ADC1 Channel 2 (GPIO 2)
 */

#ifndef RAINDROP_SENSOR_H
#define RAINDROP_SENSOR_H

#include "esp_adc/adc_oneshot.h"

/**
 * @brief Data from a single FC-37 reading
 */
typedef struct {
    int digital;  /**< DO pin: 0 = rain detected, 1 = dry */
    int analog;   /**< A0 raw ADC value (0–4095) */
} raindrop_data_t;

/**
 * @brief Initialise the FC-37 raindrop sensor.
 *
 * Configures the digital GPIO pin and the analog ADC channel.
 * The ADC unit handle is shared with other sensors on ADC1.
 *
 * @param adc_handle  Already-initialised ADC1 oneshot handle
 * @return ESP_OK on success
 */
esp_err_t RaindropSensorInit(adc_oneshot_unit_handle_t adc_handle);

/**
 * @brief Read both digital and analog outputs from the FC-37.
 *
 * @param[out] data  Pointer to struct that will be filled with the reading
 * @return ESP_OK on success
 */
esp_err_t RetrieveRaindropSensorData(raindrop_data_t *data);

/**
 * @brief Check whether the sensor indicates rain.
 *
 * @param data  Pointer to a recent reading
 * @return true if rain is detected (DO == 0)
 */
bool IsRainDetected(const raindrop_data_t *data);

#endif /* RAINDROP_SENSOR_H */
