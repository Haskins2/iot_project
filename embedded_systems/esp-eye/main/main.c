/**
 * ESP-EYE — Camera + BLE GATT Server (Merged)
 *
 * On boot: initialises the OV2640 camera and BLE stack, advertises as "ESP-EYE".
 * On-demand: waits for a BLE write to the trigger characteristic (0xFF02).
 *            Captures one JPEG frame and sends it as chunked BLE notifications
 *            on the image characteristic (0xFF01).
 *
 * Target:  ESP32-D0WD (ESP-EYE board)
 * IDF:     v5.x (Bluedroid BLE stack)
 *
 * Menuconfig requirements (already configured):
 *   Component config → Bluetooth → [*] Bluetooth
 *   Bluetooth Host   → Bluedroid
 *   Bluedroid options → [*] BLE, [*] GATTS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#include "esp_camera.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *TAG = "ESP_EYE";

/* ── BLE Configuration ──────────────────────────────────────────────────── */
#define DEVICE_NAME         "ESP-EYE"
#define SVC_UUID            0xABCD          /* Primary service UUID           */
#define CHAR_IMG_UUID       0xFF01          /* Image data characteristic      */
#define CHAR_TRIG_UUID      0xFF02          /* Trigger capture characteristic */
#define BLE_MTU_SIZE        512             /* Requested local MTU            */
#define PROFILE_APP_ID      0
#define PROFILE_NUM         1

/* ── Chunked transfer tuning ────────────────────────────────────────────── */
#define BLE_STATIC_PASSKEY  123456          /* Static passkey for SMP pairing  */
#define CHUNK_DELAY_MS      20              /* Pacing delay between BLE notifs */
#define MAX_SEND_RETRY      5               /* Retries on send_indicate fail   */
#define RETRY_DELAY_MS      50              /* Backoff between retries         */

/* ── Statistics / Logging ──────────────────────────────────────────────── */
#define STATS_TAG           "ESP_EYE_STATS"
#define STATS_SEPARATOR     "════════════════════════════════════════════════"
#define US_TO_MS(us)        ((double)(us) / 1000.0)
#define US_TO_S(us)         ((double)(us) / 1000000.0)
#define BYTES_TO_KB(b)      ((double)(b) / 1024.0)

/* ── Camera GPIO pins — ESP-EYE (ESP32-D0WD) ───────────────────────────── */
#define CAM_PIN_PWDN        -1              /* Not used                       */
#define CAM_PIN_RESET       -1              /* Not used (software reset)      */
#define CAM_PIN_XCLK         4
#define CAM_PIN_SIOD        18              /* SCCB data (I2C)                */
#define CAM_PIN_SIOC        23              /* SCCB clock (I2C)               */
#define CAM_PIN_D7          36
#define CAM_PIN_D6          37
#define CAM_PIN_D5          38
#define CAM_PIN_D4          39
#define CAM_PIN_D3          35
#define CAM_PIN_D2          14
#define CAM_PIN_D1          13
#define CAM_PIN_D0          34
#define CAM_PIN_VSYNC        5
#define CAM_PIN_HREF        27
#define CAM_PIN_PCLK        25

/* ═══════════════════════════════════════════════════════════════════════════
 *  SHARED TYPES
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Binary frame header — sent with every BLE chunk AND used by the Python
 * viewer for UART framing.  10 bytes, packed, little-endian.
 *
 * Over BLE each notification is: [frame_header_t][payload slice]
 *   magic_header = 0xFECA  (sync marker)
 *   total_len    = full JPEG size in bytes
 *   chunk_id     = 0-indexed chunk sequence number
 *   total_chunks = total chunks for this frame
 */
typedef struct __attribute__((packed)) {
    uint16_t magic_header;
    uint32_t total_len;
    uint16_t chunk_id;
    uint16_t total_chunks;
} frame_header_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  GATT ATTRIBUTE TABLE
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Attribute table indices — order must match gatt_db[] array */
enum {
    IDX_SVC,                /* Service declaration (0xABCD)             */
    IDX_IMG_CHAR_DECL,      /* Image char declaration (read + notify)   */
    IDX_IMG_CHAR_VAL,       /* Image char value (0xFF01)                */
    IDX_IMG_CHAR_CFG,       /* Image CCCD descriptor                    */
    IDX_TRIG_CHAR_DECL,     /* Trigger char declaration (write)         */
    IDX_TRIG_CHAR_VAL,      /* Trigger char value (0xFF02)              */
    IDX_NB                  /* Total attribute count = 6                */
};

static uint16_t gatt_handle_table[IDX_NB];

/* ── UUID constants for the attribute table ─────────────────────────────── */
static const uint16_t primary_service_uuid    = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t char_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t  char_prop_read_notify   = ESP_GATT_CHAR_PROP_BIT_READ
                                              | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t  char_prop_write         = ESP_GATT_CHAR_PROP_BIT_WRITE;

static const uint16_t service_uuid  = SVC_UUID;
static const uint16_t img_char_uuid = CHAR_IMG_UUID;
static const uint16_t trig_char_uuid = CHAR_TRIG_UUID;

/* CCCD initial value — notifications disabled */
static uint8_t cccd_value[2] = {0x00, 0x00};

/* Placeholder initial values */
static const uint8_t img_char_init_val[]  = {0x00};
static uint8_t trig_char_init_val[] = {0x00};

/*
 * Full GATT attribute table.  Follows the Bluedroid attr-tab pattern:
 * each entry defines one attribute in the service.
 */
static const esp_gatts_attr_db_t gatt_db[IDX_NB] = {

    /* ── Service declaration ─────────────────────────────────────────── */
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&primary_service_uuid,
            ESP_GATT_PERM_READ,
            sizeof(service_uuid),
            sizeof(service_uuid),
            (uint8_t *)&service_uuid
        }
    },

    /* ── Image characteristic declaration (read + notify) ────────────── */
    [IDX_IMG_CHAR_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&char_declaration_uuid,
            ESP_GATT_PERM_READ,
            sizeof(char_prop_read_notify),
            sizeof(char_prop_read_notify),
            (uint8_t *)&char_prop_read_notify
        }
    },

    /* ── Image characteristic value (0xFF01) ─────────────────────────── */
    [IDX_IMG_CHAR_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&img_char_uuid,
            ESP_GATT_PERM_READ_ENCRYPTED,
            512,                          /* max_length — matches MTU   */
            sizeof(img_char_init_val),
            (uint8_t *)img_char_init_val
        }
    },

    /* ── Image CCCD descriptor ───────────────────────────────────────── */
    [IDX_IMG_CHAR_CFG] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&char_client_config_uuid,
            ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
            sizeof(cccd_value),
            sizeof(cccd_value),
            (uint8_t *)cccd_value
        }
    },

    /* ── Trigger characteristic declaration (write) ──────────────────── */
    [IDX_TRIG_CHAR_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&char_declaration_uuid,
            ESP_GATT_PERM_READ,
            sizeof(char_prop_write),
            sizeof(char_prop_write),
            (uint8_t *)&char_prop_write
        }
    },

    /* ── Trigger characteristic value (0xFF02) ───────────────────────── */
    [IDX_TRIG_CHAR_VAL] = {
        {ESP_GATT_RSP_BY_APP},            /* We handle writes manually  */
        {
            ESP_UUID_LEN_16,
            (uint8_t *)&trig_char_uuid,
            ESP_GATT_PERM_WRITE_ENCRYPTED,
            sizeof(trig_char_init_val),
            sizeof(trig_char_init_val),
            (uint8_t *)trig_char_init_val
        }
    },
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  GLOBAL STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

/* BLE profile instance (same pattern as user's boilerplate) */
struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t       gatts_if;
    uint16_t       conn_id;
    bool           connected;
    bool           notify_enabled;
};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param);

static struct gatts_profile_inst profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

/* Negotiated MTU — updated in ESP_GATTS_MTU_EVT */
static uint16_t s_mtu = 23;

/* Capture task handle — used by xTaskNotifyGive to trigger capture */
static TaskHandle_t s_capture_task_handle = NULL;

/* Transfer statistics */
static uint32_t s_transfer_count = 0;   /* Running count of completed transfers */

/* ═══════════════════════════════════════════════════════════════════════════
 *  ADVERTISING DATA
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Raw advertising data:
 *   0x02 0x01 0x06       → Flags: LE General Discoverable, BR/EDR not supported
 *   0x03 0x03 0xCD 0xAB  → Complete list of 16-bit UUIDs (0xABCD, LSB first)
 */
static const uint8_t raw_adv_data[] = {
    0x02, 0x01, 0x06,
    0x03, 0x03, 0xCD, 0xAB,
};

/*
 * Scan-response carries the full device name so the client can identify us.
 *   Length byte = 1 + strlen("ESP-EYE") = 8
 *   Type 0x09 = Complete Local Name
 */
static const uint8_t raw_scan_rsp_data[] = {
    0x08, 0x09, 'E', 'S', 'P', '-', 'E', 'Y', 'E',
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,          /* 20 ms  */
    .adv_int_max        = 0x40,          /* 40 ms  */
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* Flags to coordinate GAP adv-config callbacks before starting advertising */
#define ADV_CONFIG_FLAG       (1 << 0)
#define SCAN_RSP_CONFIG_FLAG  (1 << 1)
static uint8_t adv_config_done = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  CAMERA CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

static const camera_config_t CAMERA_CONFIG = {
    .pin_pwdn     = CAM_PIN_PWDN,
    .pin_reset    = CAM_PIN_RESET,
    .pin_xclk     = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,
    .pin_d7       = CAM_PIN_D7,
    .pin_d6       = CAM_PIN_D6,
    .pin_d5       = CAM_PIN_D5,
    .pin_d4       = CAM_PIN_D4,
    .pin_d3       = CAM_PIN_D3,
    .pin_d2       = CAM_PIN_D2,
    .pin_d1       = CAM_PIN_D1,
    .pin_d0       = CAM_PIN_D0,
    .pin_vsync    = CAM_PIN_VSYNC,
    .pin_href     = CAM_PIN_HREF,
    .pin_pclk     = CAM_PIN_PCLK,

    .xclk_freq_hz = 20000000,            /* 20 MHz — standard for OV2640     */
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

// FRAMESIZE_QQVGA (160x120)
// FRAMESIZE_HQVGA (240x176)
// FRAMESIZE_QVGA (320x240) 
// FRAMESIZE_HVGA (480x320)
// FRAMESIZE_VGA (640x480)
// FRAMESIZE_SVGA (800x600)
// FRAMESIZE_XGA (1024x768)
// FRAMESIZE_HD (1280x720) — 720p
// FRAMESIZE_SXGA (1280x1024)
// FRAMESIZE_UXGA (1600x1200)

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_QVGA,        /* 320x240 — fits in DRAM          */
    .jpeg_quality = 12,                   /* 0–63, lower = better quality    */
    .fb_count     = 1,                    /* 1 framebuffer in DRAM           */
    .fb_location  = CAMERA_FB_IN_DRAM,    /* No PSRAM needed for QVGA        */
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  CAMERA FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Initialise the OV2640 camera with the static configuration.
 */
static esp_err_t camera_init(void)
{
    esp_err_t err = esp_camera_init(&CAMERA_CONFIG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
    } else {
        ESP_LOGI(TAG, "Camera initialised (QVGA JPEG, quality %d)",
                 CAMERA_CONFIG.jpeg_quality);
    }
    return err;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  BLE CHUNKED IMAGE TRANSFER
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Send a captured JPEG frame as chunked BLE notifications.
 *
 * Each notification contains:
 *   [frame_header_t (10 bytes)][JPEG payload slice (up to MTU-3-10 bytes)]
 *
 * The frame_header_t in every chunk carries the full frame metadata so the
 * receiver can detect new frames, track progress, and handle lost chunks.
 */
static void send_image_over_ble(const camera_fb_t *fb)
{
    struct gatts_profile_inst *prof = &profile_tab[PROFILE_APP_ID];

    /* Verify BLE is ready */
    if (!prof->connected || !prof->notify_enabled) {
        ESP_LOGW(TAG, "Cannot send: connected=%d, notify_enabled=%d",
                 prof->connected, prof->notify_enabled);
        return;
    }

    /* Calculate chunk geometry */
    uint16_t max_att_payload = s_mtu - 3;               /* ATT layer overhead  */
    uint16_t payload_per_chunk = max_att_payload - sizeof(frame_header_t);
    uint16_t total_chunks = (fb->len + payload_per_chunk - 1) / payload_per_chunk;

    ESP_LOGI(TAG, "Sending JPEG: %u bytes in %u chunks (MTU=%u, payload/chunk=%u)",
             (unsigned)fb->len, total_chunks, s_mtu, payload_per_chunk);

    /* Allocate a single reusable send buffer */
    uint8_t *send_buf = malloc(max_att_payload);
    if (!send_buf) {
        ESP_LOGE(TAG, "Failed to allocate send buffer (%u bytes)", max_att_payload);
        return;
    }

    for (uint16_t i = 0; i < total_chunks; i++) {
        /* Build the chunk header */
        frame_header_t hdr = {
            .magic_header = 0xFECA,
            .total_len    = fb->len,
            .chunk_id     = i,
            .total_chunks = total_chunks,
        };

        /* Calculate payload slice for this chunk */
        uint32_t offset   = (uint32_t)i * payload_per_chunk;
        uint16_t this_len = payload_per_chunk;
        if (offset + this_len > fb->len) {
            this_len = fb->len - offset;    /* Last chunk may be shorter */
        }
        uint16_t chunk_total = sizeof(frame_header_t) + this_len;

        /* Pack header + payload into send buffer */
        memcpy(send_buf, &hdr, sizeof(frame_header_t));
        memcpy(send_buf + sizeof(frame_header_t), fb->buf + offset, this_len);

        /* Send notification with retry on failure */
        esp_err_t ret = ESP_FAIL;
        for (int attempt = 0; attempt < MAX_SEND_RETRY; attempt++) {
            ret = esp_ble_gatts_send_indicate(
                prof->gatts_if,
                prof->conn_id,
                gatt_handle_table[IDX_IMG_CHAR_VAL],
                chunk_total,
                send_buf,
                false                    /* false = notification (no ACK) */
            );
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "send_indicate failed (0x%x) chunk %u, retry %d/%d",
                     ret, i, attempt + 1, MAX_SEND_RETRY);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Giving up on chunk %u after %d retries", i, MAX_SEND_RETRY);
            break;
        }

        /* Pace notifications to avoid congesting the BLE TX queue */
        vTaskDelay(pdMS_TO_TICKS(CHUNK_DELAY_MS));
    }

    free(send_buf);
    ESP_LOGI(TAG, "Image transfer complete (free heap: %lu)",
             (unsigned long)esp_get_free_heap_size());
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CAPTURE TASK
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * FreeRTOS task that blocks until signalled, then captures one JPEG frame
 * and sends it over BLE.  Signalled by xTaskNotifyGive() from the GATTS
 * write handler when the client writes to the trigger characteristic.
 */
static void capture_task(void *arg)
{
    while (1) {
        /* Block until a trigger is received */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "Capture triggered — grabbing frame...");

        /* ── Measure camera capture time ─────────────────────────────── */
        int64_t t_capture_start = esp_timer_get_time();

        camera_fb_t *fb = esp_camera_fb_get();

        int64_t t_capture_end = esp_timer_get_time();
        int64_t capture_us = t_capture_end - t_capture_start;

        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            continue;
        }

        ESP_LOGI(TAG, "Captured frame: %u bytes (%ux%u) in %.1f ms",
                 (unsigned)fb->len, fb->width, fb->height,
                 US_TO_MS(capture_us));

        /* ── Measure BLE send time ───────────────────────────────────── */
        int64_t t_send_start = esp_timer_get_time();

        send_image_over_ble(fb);

        int64_t t_send_end = esp_timer_get_time();
        int64_t send_us = t_send_end - t_send_start;

        /* ── Total end-to-end time (capture + send) ──────────────────── */
        int64_t total_us = t_send_end - t_capture_start;

        /* Increment transfer counter */
        s_transfer_count++;

        /* ── Calculate derived metrics ───────────────────────────────── */
        uint16_t payload_per_chunk = (s_mtu - 3) - sizeof(frame_header_t);
        uint16_t total_chunks = (fb->len + payload_per_chunk - 1) / payload_per_chunk;
        double   send_s      = US_TO_S(send_us);
        double   throughput  = (send_s > 0.0) ? BYTES_TO_KB(fb->len) / send_s : 0.0;
        double   theo_fps    = (US_TO_S(total_us) > 0.0)
                               ? 1.0 / US_TO_S(total_us) : 0.0;

        /* ── Print presentation-ready summary block ──────────────────── */
        ESP_LOGI(STATS_TAG, "%s", STATS_SEPARATOR);
        ESP_LOGI(STATS_TAG, "  ESP-EYE TX SUMMARY — Transfer #%lu",
                 (unsigned long)s_transfer_count);
        ESP_LOGI(STATS_TAG, "%s", STATS_SEPARATOR);
        ESP_LOGI(STATS_TAG, "  Image size       : %u bytes (%.2f KB)",
                 (unsigned)fb->len, BYTES_TO_KB(fb->len));
        ESP_LOGI(STATS_TAG, "  Resolution       : %u x %u",
                 fb->width, fb->height);
        ESP_LOGI(STATS_TAG, "  Chunks           : %u (payload %u bytes/chunk)",
                 total_chunks, payload_per_chunk);
        ESP_LOGI(STATS_TAG, "  MTU              : %u bytes",
                 s_mtu);
        ESP_LOGI(STATS_TAG, "  Header           : magic=0xFECA, hdr_size=%u bytes",
                 (unsigned)sizeof(frame_header_t));
        ESP_LOGI(STATS_TAG, "  ── Timing ──");
        ESP_LOGI(STATS_TAG, "  Camera capture   : %.1f ms",
                 US_TO_MS(capture_us));
        ESP_LOGI(STATS_TAG, "  BLE TX (all chunks): %.1f ms",
                 US_TO_MS(send_us));
        ESP_LOGI(STATS_TAG, "  Total (capture+TX) : %.1f ms",
                 US_TO_MS(total_us));
        ESP_LOGI(STATS_TAG, "  ── Derived Metrics ──");
        ESP_LOGI(STATS_TAG, "  BLE throughput   : %.2f KB/s",
                 throughput);
        ESP_LOGI(STATS_TAG, "  Theoretical FPS  : %.2f (if streaming)",
                 theo_fps);
        ESP_LOGI(STATS_TAG, "  Chunk pacing     : %d ms/chunk",
                 CHUNK_DELAY_MS);
        ESP_LOGI(STATS_TAG, "  Free heap        : %lu bytes",
                 (unsigned long)esp_get_free_heap_size());
        ESP_LOGI(STATS_TAG, "%s", STATS_SEPARATOR);

        esp_camera_fb_return(fb);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GAP EVENT HANDLER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= ~ADV_CONFIG_FLAG;
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= ~SCAN_RSP_CONFIG_FLAG;
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed, status %d",
                     param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising started");
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising stop failed, status %d",
                     param->adv_stop_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising stopped");
        }
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "Conn params updated: status=%d, min_int=%d, max_int=%d, "
                 "latency=%d, timeout=%d",
                 param->update_conn_params.status,
                 param->update_conn_params.min_int,
                 param->update_conn_params.max_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "Security request from client — accepting");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        esp_bd_addr_t bd_addr;
        memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
        ESP_LOGI(TAG, "Auth complete: addr=%02x:%02x:%02x:%02x:%02x:%02x, "
                 "success=%d, auth_mode=0x%x",
                 bd_addr[0], bd_addr[1], bd_addr[2],
                 bd_addr[3], bd_addr[4], bd_addr[5],
                 param->ble_security.auth_cmpl.success,
                 param->ble_security.auth_cmpl.auth_mode);
        if (!param->ble_security.auth_cmpl.success) {
            ESP_LOGE(TAG, "Authentication failed, reason=0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    }

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "Passkey notify: %" PRIu32, param->ble_security.key_notif.passkey);
        break;

    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GATTS PROFILE EVENT HANDLER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    /* ── Application registered — set up advertising & create attr table ── */
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "GATTS_REG_EVT, app_id=%d", param->reg.app_id);

        /* Configure raw advertisement and scan-response data */
        adv_config_done = ADV_CONFIG_FLAG | SCAN_RSP_CONFIG_FLAG;
        esp_ble_gap_config_adv_data_raw((uint8_t *)raw_adv_data,
                                        sizeof(raw_adv_data));
        esp_ble_gap_config_scan_rsp_data_raw((uint8_t *)raw_scan_rsp_data,
                                             sizeof(raw_scan_rsp_data));

        /* Create the full GATT attribute table */
        esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, IDX_NB, 0);
        break;

    /* ── Attribute table created — store handles & start service ────────── */
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Create attr table failed, status=0x%x",
                     param->add_attr_tab.status);
            break;
        }
        if (param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "Unexpected handle count %d (expected %d)",
                     param->add_attr_tab.num_handle, IDX_NB);
            break;
        }

        memcpy(gatt_handle_table, param->add_attr_tab.handles,
               sizeof(gatt_handle_table));

        ESP_LOGI(TAG, "Attr table created: svc=%d, img_val=%d, img_cccd=%d, trig_val=%d",
                 gatt_handle_table[IDX_SVC],
                 gatt_handle_table[IDX_IMG_CHAR_VAL],
                 gatt_handle_table[IDX_IMG_CHAR_CFG],
                 gatt_handle_table[IDX_TRIG_CHAR_VAL]);

        esp_ble_gatts_start_service(gatt_handle_table[IDX_SVC]);
        break;

    /* ── Service started ───────────────────────────────────────────────── */
    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "Service started, handle=%d", param->start.service_handle);
        break;

    /* ── Client connected ──────────────────────────────────────────────── */
    case ESP_GATTS_CONNECT_EVT:
        profile_tab[PROFILE_APP_ID].conn_id        = param->connect.conn_id;
        profile_tab[PROFILE_APP_ID].gatts_if       = gatts_if;
        profile_tab[PROFILE_APP_ID].connected      = true;
        profile_tab[PROFILE_APP_ID].notify_enabled = false;

        ESP_LOGI(TAG, "Client connected, conn_id=%d", param->connect.conn_id);

        /* Request a shorter connection interval for faster throughput */
        esp_ble_conn_update_params_t conn_params = {
            .min_int  = 0x06,            /* 7.5 ms  — faster for image data  */
            .max_int  = 0x10,            /* 20 ms                             */
            .latency  = 0,
            .timeout  = 400,             /* 4 s                               */
        };
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        esp_ble_gap_update_conn_params(&conn_params);
        break;

    /* ── Client disconnected — restart advertising ─────────────────────── */
    case ESP_GATTS_DISCONNECT_EVT:
        profile_tab[PROFILE_APP_ID].connected      = false;
        profile_tab[PROFILE_APP_ID].notify_enabled = false;
        ESP_LOGI(TAG, "Client disconnected, reason=0x%x", param->disconnect.reason);
        esp_ble_gap_start_advertising(&adv_params);
        break;

    /* ── MTU negotiated ────────────────────────────────────────────────── */
    case ESP_GATTS_MTU_EVT:
        s_mtu = param->mtu.mtu;
        ESP_LOGI(TAG, "MTU negotiated: %u (chunk payload = %u bytes)",
                 s_mtu, (uint16_t)(s_mtu - 3 - sizeof(frame_header_t)));
        break;

    /* ── Client wrote to a characteristic ──────────────────────────────── */
    case ESP_GATTS_WRITE_EVT: {
        uint16_t handle = param->write.handle;

        /* Case 1: CCCD write — client enabling/disabling notifications */
        if (handle == gatt_handle_table[IDX_IMG_CHAR_CFG]
            && param->write.len == 2)
        {
            uint16_t cccd = (param->write.value[1] << 8)
                          |  param->write.value[0];
            if (cccd == 0x0001) {
                profile_tab[PROFILE_APP_ID].notify_enabled = true;
                ESP_LOGI(TAG, "Notifications ENABLED by client");
            } else {
                profile_tab[PROFILE_APP_ID].notify_enabled = false;
                ESP_LOGI(TAG, "Notifications DISABLED by client");
            }
        }
        /* Case 2: Trigger write — client requesting a photo capture */
        else if (handle == gatt_handle_table[IDX_TRIG_CHAR_VAL]) {
            ESP_LOGI(TAG, "Trigger write received — requesting capture");

            /* Send write response before starting capture */
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id,
                                            ESP_GATT_OK, NULL);
            }

            /* Signal the capture task */
            if (s_capture_task_handle) {
                xTaskNotifyGive(s_capture_task_handle);
            }
        }
        break;
    }

    /* ── Notification confirmation (for indications — informational) ───── */
    case ESP_GATTS_CONF_EVT:
        if (param->conf.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "Notification confirm failed, status=0x%x",
                     param->conf.status);
        }
        break;

    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GLOBAL GATTS DISPATCH
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Routes GATTS events to the correct profile handler.
 * On registration, remembers which interface belongs to our profile.
 */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            profile_tab[PROFILE_APP_ID].gatts_if = gatts_if;
        } else {
            ESP_LOGE(TAG, "GATTS reg failed, app_id=%04x, status=%d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int i = 0; i < PROFILE_NUM; i++) {
        if (gatts_if == ESP_GATT_IF_NONE ||
            gatts_if == profile_tab[i].gatts_if)
        {
            if (profile_tab[i].gatts_cb) {
                profile_tab[i].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  APP_MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    esp_err_t ret;

    /* ── 1. NVS flash (required by the BT stack) ──────────────────────── */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 2. Initialise camera ─────────────────────────────────────────── */
    ret = camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed — halting");
        return;
    }

    /* ── 3. Release Classic BT memory (BLE-only saves ~30 KB) ─────────── */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    /* ── 4. Init and enable BT controller ─────────────────────────────── */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    /* ── 5. Init and enable Bluedroid host stack ──────────────────────── */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* ── 5b. Configure BLE security (SMP passkey pairing) ─────────────── */
    uint32_t passkey = BLE_STATIC_PASSKEY;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));

    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));

    uint8_t iocap = ESP_IO_CAP_OUT;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));

    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));

    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));

    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    /* ── 6. Register GATTS and GAP callbacks ──────────────────────────── */
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

    /* Register application profile — triggers ESP_GATTS_REG_EVT */
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_APP_ID));

    /* ── 7. Set local MTU to 512 for larger BLE payloads ──────────────── */
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(BLE_MTU_SIZE));

    /* ── 8. Create the capture task (blocked until trigger arrives) ────── */
    xTaskCreate(capture_task, "capture_task", 4096, NULL, 5,
                &s_capture_task_handle);

    ESP_LOGI(TAG, "ESP-EYE ready — BLE server advertising as \"%s\"",
             DEVICE_NAME);
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
}
