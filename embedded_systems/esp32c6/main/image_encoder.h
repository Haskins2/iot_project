/**
 * @file image_encoder.h
 * @brief Base64 encoding and MQTT JSON payload construction for JPEG images
 */

#ifndef IMAGE_ENCODER_H
#define IMAGE_ENCODER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * Build the complete MQTT JSON payload containing the base64-encoded image
 * and sensor data. The caller is responsible for freeing the returned string.
 *
 * @param jpeg_data       Pointer to the raw JPEG buffer
 * @param jpeg_len        Size of the JPEG in bytes
 * @param water_raw       Water sensor raw ADC reading
 * @param rain_digital    Raindrop sensor digital output (0 or 1)
 * @param rain_analog     Raindrop sensor analog ADC reading
 * @param out_json        Output: pointer to the allocated JSON string (caller frees)
 * @param out_json_len    Output: length of the JSON string
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t build_image_mqtt_payload(
    const uint8_t *jpeg_data,
    size_t jpeg_len,
    int water_raw,
    int rain_digital,
    int rain_analog,
    char **out_json,
    size_t *out_json_len
);

#endif /* IMAGE_ENCODER_H */
