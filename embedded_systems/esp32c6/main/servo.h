/**
 * @file servo.h
 * @brief DIYMORE-DM996 Servo motor interface (PWM via LEDC)
 *
 * PWM pin: GPIO 21
 */

#ifndef SERVO_H
#define SERVO_H

#include "esp_err.h"

/**
 * @brief Initialise the servo PWM (LEDC) peripheral.
 *
 * @return ESP_OK on success
 */
esp_err_t ServoInit(void);

/**
 * @brief Set the servo to a specific angle.
 *
 * @param angle  Desired angle in degrees (0–180), clamped internally
 */
void ActuateServo(int angle);

#endif /* SERVO_H */
