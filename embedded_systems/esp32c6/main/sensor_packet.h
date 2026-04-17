/**
 * @file sensor_packet.h
 * @brief Formats all sensor readings into a JSON packet for the cloud
 */

#ifndef SENSOR_PACKET_H
#define SENSOR_PACKET_H

#include "water_sensor.h"
#include "raindrop_sensor.h"

/**
 * @brief Format sensor data as a JSON string.
 *
 * Produces a packet like:
 * {
 *   "water_sensor":    81,
 *   "raindrop_sensor": { "digital": 0, "analog": 1392 },
 *   "ble_connected":   true,
 *   "timestamp_ms":    12345
 * }
 *
 * @param water     Water sensor reading
 * @param rain      Raindrop sensor reading
 * @param ble_ok    Whether the ESP-EYE BLE link is up
 * @param buf       Output buffer (must be at least @p buf_len bytes)
 * @param buf_len   Size of the output buffer
 * @return Number of characters written (excluding NUL), or -1 on error
 */
int FormatSensorPacket(const water_data_t *water,
                       const raindrop_data_t *rain,
                       bool ble_ok,
                       char *buf,
                       int buf_len);

#endif /* SENSOR_PACKET_H */