/**
 * @file water_sensor.h
 * @brief Water level sensor interface
 *
 * Analog output: ADC1 Channel 4 (GPIO 4)
 * Range: 0 (dry) – ~2300 (fully submerged)
 */

#ifndef WATER_SENSOR_H
#define WATER_SENSOR_H

#include "esp_adc/adc_oneshot.h"

/**
 * @brief Data from a single water sensor reading
 */
typedef struct {
    int raw;  /**< Raw ADC value (0–4095) */
} water_data_t;

/**
 * @brief Initialise the water level sensor.
 *
 * @param adc_handle  Already-initialised ADC1 oneshot handle (shared)
 * @return ESP_OK on success
 */
esp_err_t WaterSensorInit(adc_oneshot_unit_handle_t adc_handle);

/**
 * @brief Read the water sensor.
 *
 * @param[out] data  Pointer to struct that will be filled
 * @return ESP_OK on success
 */
esp_err_t RetrieveWaterSensorData(water_data_t *data);

/**
 * @brief Check whether the sensor indicates water presence.
 *
 * Uses the same threshold (300) from the original repo.
 *
 * @param data  Pointer to a recent reading
 * @return true if water is detected
 */
bool IsWaterDetected(const water_data_t *data);

#endif /* WATER_SENSOR_H */
