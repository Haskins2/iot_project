/**
 * @file image_reassembly.c
 * @brief BLE JPEG chunk reassembly for ESP-EYE image transfer
 */

#include "image_reassembly.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"

#define TAG  "img_reassembly"

#define FRAME_MAGIC  0xFECA

/* ── Static reassembly state ──────────────────────────────────────────── */

static uint8_t s_buffer[IMAGE_REASSEMBLY_MAX_SIZE];

static struct {
    bool     active;           /* Frame currently being assembled        */
    bool     complete;         /* All chunks received                    */
    uint32_t total_len;        /* Expected JPEG size from header         */
    uint16_t total_chunks;     /* Expected chunk count from header       */
    uint16_t chunks_received;  /* Number of valid chunks received so far */
    uint16_t payload_per_chunk;/* Payload size derived from first chunk  */
} s_state;

/* ===================================================================== */

esp_err_t image_reassembly_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "Initialised (buffer=%u bytes)", IMAGE_REASSEMBLY_MAX_SIZE);
    return ESP_OK;
}

esp_err_t image_reassembly_feed_chunk(const uint8_t *data, uint16_t len)
{
    if (!data || len <= sizeof(frame_header_t)) {
        ESP_LOGW(TAG, "Notification too short (%u bytes)", len);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse the header */
    const frame_header_t *hdr = (const frame_header_t *)data;

    if (hdr->magic_header != FRAME_MAGIC) {
        ESP_LOGW(TAG, "Bad magic: 0x%04X (expected 0x%04X)",
                 hdr->magic_header, FRAME_MAGIC);
        return ESP_ERR_INVALID_ARG;
    }

    if (hdr->total_len > IMAGE_REASSEMBLY_MAX_SIZE) {
        ESP_LOGE(TAG, "JPEG too large: %" PRIu32 " bytes (max %u)",
                 hdr->total_len, IMAGE_REASSEMBLY_MAX_SIZE);
        return ESP_ERR_INVALID_ARG;
    }

    if (hdr->chunk_id >= hdr->total_chunks) {
        ESP_LOGW(TAG, "chunk_id %u >= total_chunks %u",
                 hdr->chunk_id, hdr->total_chunks);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t payload_len = len - sizeof(frame_header_t);

    /* First chunk of a new frame — initialise state */
    if (hdr->chunk_id == 0) {
        if (s_state.active && !s_state.complete) {
            ESP_LOGW(TAG, "Discarding incomplete frame (%u/%u chunks received)",
                     s_state.chunks_received, s_state.total_chunks);
        }

        s_state.active           = true;
        s_state.complete         = false;
        s_state.total_len        = hdr->total_len;
        s_state.total_chunks     = hdr->total_chunks;
        s_state.chunks_received  = 0;
        s_state.payload_per_chunk = payload_len;

        ESP_LOGI(TAG, "New frame: %" PRIu32 " bytes, %u chunks",
                 hdr->total_len, hdr->total_chunks);
    }

    /* Ignore chunks if no frame is active or frame is already complete */
    if (!s_state.active || s_state.complete) {
        return ESP_OK;
    }

    /* Validate consistency with the current frame */
    if (hdr->total_len != s_state.total_len ||
        hdr->total_chunks != s_state.total_chunks)
    {
        ESP_LOGW(TAG, "Metadata mismatch — ignoring chunk %u", hdr->chunk_id);
        return ESP_ERR_INVALID_ARG;
    }

    /* Calculate write offset and copy payload */
    uint32_t offset = (uint32_t)hdr->chunk_id * s_state.payload_per_chunk;

    if (offset + payload_len > IMAGE_REASSEMBLY_MAX_SIZE) {
        ESP_LOGE(TAG, "Chunk %u would overflow buffer (offset=%" PRIu32 ", len=%u)",
                 hdr->chunk_id, offset, payload_len);
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_buffer + offset, data + sizeof(frame_header_t), payload_len);
    s_state.chunks_received++;

    /* Log progress every 10 chunks */
    if (s_state.chunks_received % 10 == 0) {
        ESP_LOGI(TAG, "Progress: %u/%u chunks",
                 s_state.chunks_received, s_state.total_chunks);
    }

    /* Check for completion */
    if (s_state.chunks_received == s_state.total_chunks) {
        s_state.complete = true;
        ESP_LOGI(TAG, "Frame complete: %" PRIu32 " bytes in %u chunks",
                 s_state.total_len, s_state.total_chunks);
    }

    return ESP_OK;
}

bool image_reassembly_is_complete(void)
{
    return s_state.active && s_state.complete;
}

esp_err_t image_reassembly_get_image(const uint8_t **out_data, size_t *out_len)
{
    if (!image_reassembly_is_complete()) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_data = s_buffer;
    *out_len  = s_state.total_len;
    return ESP_OK;
}

void image_reassembly_reset(void)
{
    s_state.active   = false;
    s_state.complete = false;
    s_state.chunks_received = 0;
    ESP_LOGI(TAG, "Reset — ready for next frame");
}
