/**
 * @file image_reassembly.h
 * @brief BLE JPEG chunk reassembly for ESP-EYE image transfer
 *
 * Receives chunked BLE notifications containing frame_header_t + JPEG
 * payload slices and reassembles them into a contiguous JPEG buffer.
 */

#ifndef IMAGE_REASSEMBLY_H
#define IMAGE_REASSEMBLY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define IMAGE_REASSEMBLY_MAX_SIZE  (20 * 1024)  /* 20KB max JPEG — generous for QVGA */

/**
 * Binary frame header — sent with every BLE chunk by the ESP-EYE.
 * 10 bytes, packed, little-endian.
 */
typedef struct __attribute__((packed)) {
    uint16_t magic_header;   /**< Always 0xFECA (sync marker) */
    uint32_t total_len;      /**< Full JPEG size in bytes     */
    uint16_t chunk_id;       /**< 0-indexed chunk sequence    */
    uint16_t total_chunks;   /**< Total chunks for this frame */
} frame_header_t;

/**
 * Initialise the reassembly module. Call once at startup.
 */
esp_err_t image_reassembly_init(void);

/**
 * Feed a raw BLE notification into the reassembly buffer.
 * Parses the frame_header_t, validates it, and copies the payload
 * slice into the correct offset in the reassembly buffer.
 *
 * @param data   Raw notification data (header + payload)
 * @param len    Length of the notification data
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if header is malformed
 */
esp_err_t image_reassembly_feed_chunk(const uint8_t *data, uint16_t len);

/**
 * Check if the current frame is fully reassembled.
 * @return true if all chunks for the current frame have been received
 */
bool image_reassembly_is_complete(void);

/**
 * Get a pointer to the reassembled JPEG data and its size.
 * Only valid when image_reassembly_is_complete() returns true.
 *
 * @param out_data  Pointer to the JPEG buffer (do NOT free this)
 * @param out_len   Size of the JPEG in bytes
 * @return ESP_OK if image is available, ESP_ERR_INVALID_STATE if not complete
 */
esp_err_t image_reassembly_get_image(const uint8_t **out_data, size_t *out_len);

/**
 * Reset the reassembly state for a new frame.
 * Call this after you've consumed the completed image.
 */
void image_reassembly_reset(void);

#endif /* IMAGE_REASSEMBLY_H */
