/**
 * @file ble_client.h
 * @brief BLE GATT Client for ESP-EYE camera communication
 *
 * Scans for "ESP-EYE", connects, discovers service 0xABCD with two chars:
 *   - 0xFF01: Image data (notifications — receives JPEG chunks)
 *   - 0xFF02: Trigger   (write — sends capture command)
 *
 * Handles disconnections gracefully — automatically restarts scanning.
 */

#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialise the BLE stack and begin scanning for the ESP-EYE.
 *
 * This is non-blocking; scanning and connection happen asynchronously
 * via BLE callbacks.
 *
 * @return ESP_OK on success
 */
esp_err_t BleClientInit(void);

/**
 * @brief Check whether the ESP-EYE is currently connected.
 *
 * @return true if connected and characteristic has been discovered
 */
bool IsBleConnected(void);

/**
 * @brief Write a trigger command to the ESP-EYE's 0xFF02 characteristic.
 *
 * The ESP-EYE server will capture a JPEG frame and stream it back
 * as chunked notifications on 0xFF01.
 *
 * If the ESP-EYE is not connected, this logs a warning and returns
 * ESP_ERR_INVALID_STATE (does not block or crash).
 *
 * @return ESP_OK if the write was issued, ESP_ERR_INVALID_STATE if
 *         not connected
 */
esp_err_t TriggerCameraCapture(void);

#endif /* BLE_CLIENT_H */