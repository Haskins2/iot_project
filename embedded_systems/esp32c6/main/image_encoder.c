/**
 * @file image_encoder.c
 * @brief Base64 encoding and MQTT JSON payload construction for JPEG images
 *
 * Uses mbedtls base64 (built into ESP-IDF) — no external dependencies.
 * Builds JSON manually to minimise memory usage.
 */

#include "image_encoder.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "mbedtls/base64.h"

#define TAG  "img_encoder"

/*
 * JSON template (the base64 string is inserted between the two halves):
 *
 * {"image_base64":"<b64>","image_size_bytes":<N>,
 *  "water_sensor":<N>,"raindrop_sensor":{"digital":<N>,"analog":<N>},
 *  "timestamp_ms":<N>}
 *
 * Maximum overhead for the JSON wrapper (excluding base64 data):
 *   Fixed text ~160 bytes + up to 5 * 11 digits for integers = ~215 bytes.
 *   Use 256 as a generous upper bound.
 */
#define JSON_WRAPPER_MAX  256

esp_err_t build_image_mqtt_payload(
    const uint8_t *jpeg_data,
    size_t jpeg_len,
    int water_raw,
    int rain_digital,
    int rain_analog,
    char **out_json,
    size_t *out_json_len)
{
    *out_json     = NULL;
    *out_json_len = 0;

    /* ── 1. Calculate base64 output size ──────────────────────────────── */
    size_t b64_len = 0;
    int ret = mbedtls_base64_encode(NULL, 0, &b64_len, jpeg_data, jpeg_len);
    /* mbedtls returns MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL when called
       with a NULL buffer — this is expected; b64_len is now set.         */
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGE(TAG, "base64 size query failed: %d", ret);
        return ESP_ERR_NO_MEM;
    }

    /* ── 2. Allocate the final JSON buffer (wrapper + base64) ─────────── */
    size_t json_buf_size = JSON_WRAPPER_MAX + b64_len;
    char *json = malloc(json_buf_size);
    if (!json) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer (%u bytes)",
                 (unsigned)json_buf_size);
        return ESP_ERR_NO_MEM;
    }

    /* ── 3. Write JSON prefix ─────────────────────────────────────────── */
    int prefix_len = snprintf(json, json_buf_size,
        "{\"image_base64\":\"");
    if (prefix_len < 0) {
        free(json);
        return ESP_ERR_NO_MEM;
    }

    /* ── 4. Encode base64 directly into the JSON buffer ───────────────── */
    size_t b64_written = 0;
    ret = mbedtls_base64_encode(
        (unsigned char *)(json + prefix_len),
        json_buf_size - prefix_len,
        &b64_written,
        jpeg_data,
        jpeg_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "base64 encode failed: %d", ret);
        free(json);
        return ESP_ERR_NO_MEM;
    }

    /* ── 5. Write JSON suffix after the base64 data ───────────────────── */
    uint32_t timestamp = esp_log_timestamp();
    int suffix_len = snprintf(
        json + prefix_len + b64_written,
        json_buf_size - prefix_len - b64_written,
        "\",\"image_size_bytes\":%u,"
        "\"water_sensor\":%d,"
        "\"raindrop_sensor\":{\"digital\":%d,\"analog\":%d},"
        "\"timestamp_ms\":%" PRIu32 "}",
        (unsigned)jpeg_len,
        water_raw,
        rain_digital,
        rain_analog,
        timestamp);
    if (suffix_len < 0) {
        free(json);
        return ESP_ERR_NO_MEM;
    }

    size_t total_len = prefix_len + b64_written + suffix_len;

    ESP_LOGI(TAG, "JSON payload built: %u bytes (JPEG=%u, base64=%u)",
             (unsigned)total_len, (unsigned)jpeg_len, (unsigned)b64_written);

    *out_json     = json;
    *out_json_len = total_len;
    return ESP_OK;
}
