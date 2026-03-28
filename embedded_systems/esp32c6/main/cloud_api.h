/**
 * @file cloud_api.h
 * @brief Cloud-facing API for the ESP32-C6 flood detection system
 *
 * Consolidates all functions the cloud/MQTT layer needs to call.
 * Each function is safe to call from any task context.
 *
 * ── Actuation ──────────────────────────────────────────────────
 *   ActuateServo(angle)        — Set servo angle (0–180°)        [servo.h]
 *   PumpOn() / PumpOff()       — Toggle water pump               [pump.h]
 *   TriggerCameraCapture()     — Request photo from ESP-EYE      [ble_client.h]
 *
 * ── Sensor readings ────────────────────────────────────────────
 *   RetrieveWaterSensorData()  — Read water level (ADC)          [water_sensor.h]
 *   RetrieveRaindropSensorData() — Read rain sensor (digital+ADC)[raindrop_sensor.h]
 *   IsWaterDetected()          — Threshold check                 [water_sensor.h]
 *   IsRainDetected()           — Threshold check                 [raindrop_sensor.h]
 *
 * ── Configuration ──────────────────────────────────────────────
 *   SetPollIntervalMs(ms)      — Change sensor polling rate      [this file]
 *   GetPollIntervalMs()        — Query current polling rate      [this file]
 *
 * ── BLE status ─────────────────────────────────────────────────
 *   IsBleConnected()           — Check ESP-EYE link              [ble_client.h]
 */

#ifndef CLOUD_API_H
#define CLOUD_API_H

#include <stdint.h>

/* Re-export all subsystem headers the cloud layer needs */
#include "servo.h"
#include "pump.h"
#include "ble_client.h"
#include "water_sensor.h"
#include "raindrop_sensor.h"

/**
 * @brief Set the sensor polling interval.
 *
 * Takes effect on the next loop iteration. Clamped to a minimum of 100 ms.
 *
 * @param interval_ms  Desired interval in milliseconds
 */
void SetPollIntervalMs(uint32_t interval_ms);

/**
 * @brief Get the current sensor polling interval.
 *
 * @return Current interval in milliseconds
 */
uint32_t GetPollIntervalMs(void);

#endif /* CLOUD_API_H */
