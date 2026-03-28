/**
 * @file pump.h
 * @brief Water pump interface (digital on/off via GPIO 18)
 *
 * Output pin: GPIO 18 — HIGH = pump on, LOW = pump off
 */

#ifndef PUMP_H
#define PUMP_H

#include "esp_err.h"

/**
 * @brief Initialise the pump GPIO as a digital output (default OFF).
 *
 * @return ESP_OK on success
 */
esp_err_t PumpInit(void);

/**
 * @brief Turn the pump on (GPIO 18 HIGH — 3.3V).
 */
void PumpOn(void);

/**
 * @brief Turn the pump off (GPIO 18 LOW).
 */
void PumpOff(void);

#endif /* PUMP_H */
