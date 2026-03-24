/**
 * ESP32-C6 — BLE GATT Client + Water Sensor + Servo + Image Reassembly
 *
 * This firmware combines three responsibilities:
 *   1. Water sensing (ADC) + servo actuation (LEDC PWM) — flood detection
 *   2. BLE GATT client  — connects to ESP-EYE, triggers photo capture
 *   3. Image reassembly  — rebuilds JPEG from BLE chunks, forwards via console UART
 *
 * The water sensor runs in its own FreeRTOS task.  BLE runs in callbacks.
 * When a complete JPEG is reassembled from BLE notifications, it is forwarded
 * over the console UART (UART0) using the same frame_header_t protocol that
 * view_camera.py already understands.  Binary data is written directly to the
 * UART0 hardware FIFO via esp_rom_uart_tx_one_char(), with ESP_LOG output
 * temporarily suppressed to prevent interleaving.
 *
 * Target:  ESP32-C6 (BLE-only, no Classic BT)
 * IDF:     v5.x (Bluedroid BLE stack)
 *
 * Menuconfig requirements:
 *   Component config → Bluetooth → [*] Bluetooth
 *   Bluetooth Host   → Bluedroid
 *   Bluedroid options → [*] BLE, [*] GATTC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "esp_rom_uart.h"
#include "mbedtls/base64.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *TAG = "ESP32_C6";

/* ── BLE Configuration ──────────────────────────────────────────────────── */
#define REMOTE_DEVICE_NAME      "ESP-EYE"
#define SVC_UUID                0xABCD      /* Must match server               */
#define CHAR_IMG_UUID           0xFF01      /* Image data (notify)             */
#define CHAR_TRIG_UUID          0xFF02      /* Trigger capture (write)         */
#define BLE_MTU_SIZE            512         /* Requested local MTU             */
#define PROFILE_APP_ID          0
#define PROFILE_NUM             1
#define SCAN_DURATION_SEC       30          /* Scan window before restart      */

/* ── Image Reassembly ───────────────────────────────────────────────────── */
#define IMG_BUF_SIZE            (64 * 1024) /* Max JPEG size (64 KB) — fits 720p JPEG */
#define REASSEMBLY_TIMEOUT_MS   (5 * 1000) /* Discard partial frame after 5s  */

/* ── Statistics / Logging ──────────────────────────────────────────────── */
#define STATS_TAG           "ESP32_C6_STATS"
#define STATS_SEPARATOR     "════════════════════════════════════════════════"
#define US_TO_MS(us)        ((double)(us) / 1000.0)
#define US_TO_S(us)         ((double)(us) / 1000000.0)
#define BYTES_TO_KB(b)      ((double)(b) / 1024.0)

/* ── UART Forwarding (Console UART0) ───────────────────────────────────── */
/*
 * Binary JPEG data is sent over the console UART (UART0) — the same
 * serial port used for flashing and log output.  This avoids the need
 * for a separate USB-to-UART adapter.
 *
 * How it works:
 *   - esp_rom_uart_tx_one_char() writes raw bytes directly to the UART0
 *     hardware FIFO, bypassing the VFS/console driver (no conflicts).
 *   - ESP_LOG output is suppressed during binary transfer to prevent
 *     ASCII log text from interleaving with the JPEG data stream.
 *   - Baud rate = whatever the console is configured at (default 115200).
 *
 * Usage:
 *   1. Close idf_monitor (Ctrl-]) — it holds the serial port
 *   2. Run:  python view_camera.py --port /dev/tty.usbserial-210 --baud 115200
 *   3. The viewer syncs on magic bytes (0xCA 0xFE) and ignores log text
 */

/* ── Water Sensor (ADC) — unchanged from original code ──────────────────── */
#define WATER_SENSOR_ADC_CHANNEL    ADC_CHANNEL_4
#define WATER_SENSOR_ADC_UNIT       ADC_UNIT_1
#define WATER_SENSOR_ADC_ATTEN      ADC_ATTEN_DB_12  /* Full-scale ~3.3V      */
#define WATER_DETECTION_THRESHOLD   300               /* ADC threshold         */

/* ── Servo Motor (PWM via LEDC) — unchanged from original code ──────────── */
#define SERVO_PIN               21
#define SERVO_CHANNEL           LEDC_CHANNEL_0
#define SERVO_TIMER             LEDC_TIMER_0
#define SERVO_FREQ              50          /* Standard servo frequency (Hz)   */
#define SERVO_RESOLUTION        LEDC_TIMER_16_BIT
#define SERVO_MIN_PULSE_US      500
#define SERVO_MAX_PULSE_US      2500

/* ── Timing ─────────────────────────────────────────────────────────────── */
#define SENSOR_READ_INTERVAL_MS 1000
#define PHOTO_COOLDOWN_US       (15000000LL)  /* 15 seconds */

/* ═══════════════════════════════════════════════════════════════════════════
 *  FORWARD DECLARATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */
static void request_photo(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  SHARED TYPES
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Binary frame header — used for both BLE chunking and UART forwarding.
 * 10 bytes, packed, little-endian.  Must match the ESP-EYE server and
 * the Python viewer (view_camera.py).
 */
typedef struct __attribute__((packed)) {
    uint16_t magic_header;     /* 0xFECA                                  */
    uint32_t total_len;        /* Full JPEG size in bytes                  */
    uint16_t chunk_id;         /* 0-indexed chunk sequence (BLE)           */
    uint16_t total_chunks;     /* Total chunks in frame (BLE)              */
} frame_header_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  GLOBAL STATE — BLE
 * ═══════════════════════════════════════════════════════════════════════════ */

/* GATTC profile instance (same pattern as user's boilerplate) */
struct gattc_profile_inst {
    esp_gattc_cb_t  gattc_cb;
    uint16_t        gattc_if;
    uint16_t        conn_id;
    uint16_t        service_start_handle;
    uint16_t        service_end_handle;
    esp_bd_addr_t   remote_bda;
    bool            connected;
    bool            service_found;
};

static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param);

static struct gattc_profile_inst profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] = {
        .gattc_cb = gattc_profile_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,
    },
};

/* Negotiated MTU — updated in ESP_GATTC_CFG_MTU_EVT */
static uint16_t s_mtu = 23;

/* Characteristic handles — discovered at runtime */
static uint16_t s_img_char_handle  = 0;    /* 0xFF01 value handle             */
static uint16_t s_trig_char_handle = 0;    /* 0xFF02 value handle             */
static bool     s_img_char_found   = false;
static bool     s_trig_char_found  = false;

/* ═══════════════════════════════════════════════════════════════════════════
 *  GLOBAL STATE — IMAGE REASSEMBLY
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t       *s_img_buf              = NULL;   /* Reassembly buffer  */
static uint32_t       s_img_expected_len     = 0;
static uint16_t       s_img_expected_chunks  = 0;
static uint16_t       s_img_received_chunks  = 0;
static bool           s_img_reassembly_active = false;
static TimerHandle_t  s_reassembly_timer     = NULL;

/* ── Transfer timing & statistics ──────────────────────────────────────── */
static int64_t  s_ble_rx_start_us   = 0;     /* Timestamp of first chunk received  */
static int64_t  s_ble_rx_end_us     = 0;     /* Timestamp of last chunk received   */
static uint32_t s_frame_count       = 0;     /* Running count of completed frames  */

/* ═══════════════════════════════════════════════════════════════════════════
 *  BLE SCAN PARAMETERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,       /* 50 ms */
    .scan_window        = 0x30,       /* 30 ms */
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

/* UUID filters for service/characteristic discovery */
static esp_bt_uuid_t remote_filter_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = SVC_UUID },
};

static esp_bt_uuid_t remote_filter_img_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = CHAR_IMG_UUID },
};

static esp_bt_uuid_t remote_filter_trig_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = CHAR_TRIG_UUID },
};

static esp_bt_uuid_t notify_descr_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  WATER SENSOR + SERVO  (preserved from original code)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Set the servo angle (0–180 degrees).
 * Converts angle to PWM duty cycle based on min/max pulse widths.
 */
static void set_servo_angle(int angle)
{
    /* Clamp angle to valid range */
    if (angle < 0)   angle = 0;
    if (angle > 180) angle = 180;

    /* Convert pulse width (us) to duty cycle ticks
     * Formula: ticks = (pulse_us / period_us) * max_ticks
     * At 50Hz: period = 20000us, max_ticks = 65536 (16-bit) */
    uint32_t min_duty = (SERVO_MIN_PULSE_US * 65536) / 20000;
    uint32_t max_duty = (SERVO_MAX_PULSE_US * 65536) / 20000;
    uint32_t duty     = min_duty + ((angle * (max_duty - min_duty)) / 180);

    ESP_LOGI(TAG, "Servo: angle=%d, duty=%lu", angle, (unsigned long)duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL);
}

/**
 * FreeRTOS task: reads water sensor ADC every 1 second and actuates servo.
 * Runs independently from BLE — the existing behaviour is fully preserved.
 */
static void water_sensor_task(void *arg)
{
    /* ── ADC initialisation (moved from app_main) ─────────────────────── */
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = WATER_SENSOR_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = WATER_SENSOR_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle,
                                               WATER_SENSOR_ADC_CHANNEL,
                                               &config));

    /* ── LEDC (servo PWM) initialisation (moved from app_main) ────────── */
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_RESOLUTION,
        .timer_num       = SERVO_TIMER,
        .freq_hz         = SERVO_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t channel_conf = {
        .gpio_num   = SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = SERVO_CHANNEL,
        .timer_sel  = SERVO_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));

    ESP_LOGI(TAG, "Water sensor task started (ADC ch%d, servo GPIO%d)",
             WATER_SENSOR_ADC_CHANNEL, SERVO_PIN);

    int64_t last_photo_time_us = 0;

    /* ── Main sensing loop (identical logic to original code) ─────────── */
    while (1) {
        int adc_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle,
                                         WATER_SENSOR_ADC_CHANNEL,
                                         &adc_raw));
        ESP_LOGI(TAG, "Water Sensor Raw Value: %d", adc_raw);

        if (adc_raw > WATER_DETECTION_THRESHOLD) {
            ESP_LOGI(TAG, "Water detected! Opening servo.");
            set_servo_angle(90);

            int64_t now_us = esp_timer_get_time();
            /* Trigger photo immediately on first detection, then enforce 15s cooldown */
            if (last_photo_time_us == 0 || (now_us - last_photo_time_us) >= PHOTO_COOLDOWN_US) {
                ESP_LOGI(TAG, "Triggering photo after water detection...");
                request_photo();
                last_photo_time_us = now_us;
            } else {
                double remaining_s = (double)(PHOTO_COOLDOWN_US - (now_us - last_photo_time_us)) / 1000000.0;
                ESP_LOGD(TAG, "Photo on cooldown (%.1f s remaining)", remaining_s);
            }
        } else {
            set_servo_angle(0);
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  UART FORWARDING (Console UART0 — ROM-level direct writes)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Forward a complete JPEG image to the host via the console UART (UART0).
 *
 * Uses the same frame_header_t protocol that view_camera.py expects:
 *   [frame_header_t (10 bytes): magic=0xFECA, len=N, chunk=0, total=1]
 *   [JPEG data (N bytes)]
 *
 * Implementation:
 *   1. Suppress all ESP_LOG output (prevent ASCII/binary interleave)
 *   2. Wait for any pending console bytes to finish transmitting
 *   3. Write frame header + JPEG payload byte-by-byte via ROM function
 *   4. Wait for all bytes to drain from the TX FIFO
 *   5. Re-enable ESP_LOG output
 *
 * esp_rom_uart_tx_one_char() writes directly to the UART0 hardware FIFO,
 * bypassing the VFS/console driver — no conflicts, no newline translation.
 */
static void uart_forward_image(const uint8_t *jpeg_data, uint32_t jpeg_len)
{
    /* ── Calculate Base64 size ────────────────────────────────────────── */
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, jpeg_data, jpeg_len);

    unsigned char *b64_buf = malloc(olen + 1);
    if (!b64_buf) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer (%zu bytes)", olen);
        return;
    }

    /* ── Encode directly to buffer ────────────────────────────────────── */
    int ret = mbedtls_base64_encode(b64_buf, olen, &olen, jpeg_data, jpeg_len);
    if (ret == 0) {
        b64_buf[olen] = '\0';
        
        /* Disable automatic ESP_LOG prefixing briefly to send raw block */
        esp_log_level_set(TAG, ESP_LOG_NONE);
        
        printf("\n[START_IMG]\n");
        
        /* Print in chunks to avoid overwhelming the UART buffer */
        const size_t print_chunk = 512;
        for (size_t i = 0; i < olen; i += print_chunk) {
            size_t to_print = (i + print_chunk < olen) ? print_chunk : (olen - i);
            printf("%.*s", (int)to_print, b64_buf + i);
            /* Yield slightly to let UART hardware flush if needed, completely optional but safe */
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        
        printf("\n[END_IMG]\n");
        
        esp_log_level_set(TAG, ESP_LOG_INFO);
        ESP_LOGI(TAG, "Forwarded %u bytes as Base64 to Console UART", (unsigned)jpeg_len);
    } else {
        ESP_LOGE(TAG, "Base64 encode failed: %d", ret);
    }

    free(b64_buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  IMAGE REASSEMBLY FROM BLE CHUNKS
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Timeout callback — fires if we haven't received all chunks within
 * REASSEMBLY_TIMEOUT_MS.  Discards the partial frame.
 */
static void reassembly_timeout_cb(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG, "Reassembly timeout — discarding partial frame "
             "(%u/%u chunks received)",
             s_img_received_chunks, s_img_expected_chunks);

    s_img_reassembly_active = false;
    s_img_received_chunks   = 0;
    s_img_expected_len      = 0;
    s_img_expected_chunks   = 0;
}

/**
 * Process one BLE notification containing an image chunk.
 *
 * Each notification is:  [frame_header_t (10 bytes)][JPEG payload slice]
 *
 * Called from ESP_GATTC_NOTIFY_EVT for every notification on char 0xFF01.
 */
static void handle_image_chunk(const uint8_t *data, uint16_t data_len)
{
    /* ── Validate minimum size ────────────────────────────────────────── */
    if (data_len < sizeof(frame_header_t)) {
        ESP_LOGW(TAG, "Chunk too small: %u bytes (need %u header)",
                 data_len, (unsigned)sizeof(frame_header_t));
        return;
    }

    /* ── Parse chunk header ───────────────────────────────────────────── */
    frame_header_t hdr;
    memcpy(&hdr, data, sizeof(frame_header_t));

    if (hdr.magic_header != 0xFECA) {
        ESP_LOGW(TAG, "Bad magic: 0x%04X (expected 0xFECA)", hdr.magic_header);
        return;
    }

    /* Sanity checks */
    if (hdr.total_len == 0 || hdr.total_len > IMG_BUF_SIZE) {
        ESP_LOGW(TAG, "Invalid total_len: %u (max %d)",
                 (unsigned)hdr.total_len, IMG_BUF_SIZE);
        return;
    }
    if (hdr.chunk_id >= hdr.total_chunks) {
        ESP_LOGW(TAG, "Invalid chunk_id: %u >= total_chunks %u",
                 hdr.chunk_id, hdr.total_chunks);
        return;
    }

    /* ── Start of new frame (chunk 0) ─────────────────────────────────── */
    if (hdr.chunk_id == 0) {
        if (s_img_reassembly_active) {
            ESP_LOGW(TAG, "New frame started, discarding previous partial "
                     "(%u/%u chunks)", s_img_received_chunks, s_img_expected_chunks);
        }

        s_img_expected_len    = hdr.total_len;
        s_img_expected_chunks = hdr.total_chunks;
        s_img_received_chunks = 0;
        s_img_reassembly_active = true;

        /* Record BLE reception start time */
        s_ble_rx_start_us = esp_timer_get_time();

        /* Allocate buffer on first use, reuse thereafter */
        if (!s_img_buf) {
            s_img_buf = malloc(IMG_BUF_SIZE);
            if (!s_img_buf) {
                ESP_LOGE(TAG, "Failed to allocate image buffer (%d bytes)",
                         IMG_BUF_SIZE);
                s_img_reassembly_active = false;
                return;
            }
            ESP_LOGI(TAG, "Image buffer allocated: %d bytes", IMG_BUF_SIZE);
        }

        /* Start (or reset) the timeout timer */
        xTimerReset(s_reassembly_timer, 0);

        ESP_LOGI(TAG, "Receiving frame: %u bytes in %u chunks",
                 (unsigned)hdr.total_len, hdr.total_chunks);
    }

    /* ── Validate ongoing reassembly ──────────────────────────────────── */
    if (!s_img_reassembly_active) {
        ESP_LOGW(TAG, "Chunk %u received but no active reassembly", hdr.chunk_id);
        return;
    }

    /* Check frame metadata consistency */
    if (hdr.total_len != s_img_expected_len ||
        hdr.total_chunks != s_img_expected_chunks)
    {
        ESP_LOGW(TAG, "Frame metadata mismatch (len: %u vs %u, chunks: %u vs %u)",
                 (unsigned)hdr.total_len, (unsigned)s_img_expected_len,
                 hdr.total_chunks, s_img_expected_chunks);
        s_img_reassembly_active = false;
        xTimerStop(s_reassembly_timer, 0);
        return;
    }

    /* ── Copy payload into reassembly buffer ──────────────────────────── */
    uint16_t payload_per_chunk = (s_mtu - 3) - sizeof(frame_header_t);
    uint32_t offset     = (uint32_t)hdr.chunk_id * payload_per_chunk;
    uint16_t payload_len = data_len - sizeof(frame_header_t);

    /* Bounds check */
    if (offset + payload_len > s_img_expected_len) {
        ESP_LOGW(TAG, "Chunk %u overflows buffer (offset=%u, payload=%u, total=%u)",
                 hdr.chunk_id, (unsigned)offset, payload_len,
                 (unsigned)s_img_expected_len);
        s_img_reassembly_active = false;
        xTimerStop(s_reassembly_timer, 0);
        return;
    }

    memcpy(s_img_buf + offset, data + sizeof(frame_header_t), payload_len);
    s_img_received_chunks++;

    /* Log progress every 10 chunks or on the last chunk */
    if (s_img_received_chunks % 10 == 0 ||
        s_img_received_chunks == s_img_expected_chunks)
    {
        ESP_LOGI(TAG, "Reassembly progress: %u/%u chunks",
                 s_img_received_chunks, s_img_expected_chunks);
    }

    /* ── Check if frame is complete ───────────────────────────────────── */
    if (s_img_received_chunks == s_img_expected_chunks) {
        xTimerStop(s_reassembly_timer, 0);

        /* ── Record BLE reception end time ─────────────────────────── */
        s_ble_rx_end_us = esp_timer_get_time();
        int64_t ble_rx_us = s_ble_rx_end_us - s_ble_rx_start_us;

        ESP_LOGI(TAG, "Frame complete: %u bytes, BLE RX took %.1f ms",
                 (unsigned)s_img_expected_len, US_TO_MS(ble_rx_us));

        /* ── Measure UART forwarding time ──────────────────────────── */
        int64_t t_uart_start = esp_timer_get_time();

        uart_forward_image(s_img_buf, s_img_expected_len);

        int64_t t_uart_end = esp_timer_get_time();
        int64_t uart_us = t_uart_end - t_uart_start;

        /* ── Total time: BLE reception + UART forwarding ───────────── */
        int64_t total_us = t_uart_end - s_ble_rx_start_us;

        /* Increment frame counter */
        s_frame_count++;

        /* ── Calculate derived metrics ─────────────────────────────── */
        uint16_t payload_per_chunk = (s_mtu - 3) - sizeof(frame_header_t);
        double   ble_rx_s    = US_TO_S(ble_rx_us);
        double   ble_tput    = (ble_rx_s > 0.0)
                               ? BYTES_TO_KB(s_img_expected_len) / ble_rx_s : 0.0;
        double   uart_s      = US_TO_S(uart_us);
        double   uart_tput   = (uart_s > 0.0)
                               ? BYTES_TO_KB(s_img_expected_len) / uart_s : 0.0;
        double   theo_fps    = (US_TO_S(total_us) > 0.0)
                               ? 1.0 / US_TO_S(total_us) : 0.0;

        /* Theoretical UART throughput limit at current baud
         * 115200 baud, 10 bits/byte (8N1) = 11520 bytes/s = 11.25 KB/s */
        double uart_baud_kbps = 115200.0 / 10.0 / 1024.0;

        /* ── Print presentation-ready summary block ────────────────── */
        ESP_LOGI(STATS_TAG, "%s", STATS_SEPARATOR);
        ESP_LOGI(STATS_TAG, "  ESP32-C6 RX SUMMARY — Frame #%lu",
                 (unsigned long)s_frame_count);
        ESP_LOGI(STATS_TAG, "%s", STATS_SEPARATOR);
        ESP_LOGI(STATS_TAG, "  Image size        : %u bytes (%.2f KB)",
                 (unsigned)s_img_expected_len,
                 BYTES_TO_KB(s_img_expected_len));
        ESP_LOGI(STATS_TAG, "  Chunks received   : %u / %u",
                 s_img_received_chunks, s_img_expected_chunks);
        ESP_LOGI(STATS_TAG, "  Payload/chunk     : %u bytes",
                 payload_per_chunk);
        ESP_LOGI(STATS_TAG, "  MTU (negotiated)  : %u bytes",
                 s_mtu);
        ESP_LOGI(STATS_TAG, "  Header            : magic=0xFECA, hdr_size=%u bytes",
                 (unsigned)sizeof(frame_header_t));
        ESP_LOGI(STATS_TAG, "  ── Timing ──");
        ESP_LOGI(STATS_TAG, "  BLE RX time       : %.1f ms (first→last chunk)",
                 US_TO_MS(ble_rx_us));
        ESP_LOGI(STATS_TAG, "  UART TX time      : %.1f ms (%u bytes @ 115200 baud)",
                 US_TO_MS(uart_us), (unsigned)s_img_expected_len);
        ESP_LOGI(STATS_TAG, "  Total (RX + UART) : %.1f ms",
                 US_TO_MS(total_us));
        ESP_LOGI(STATS_TAG, "  ── Derived Metrics ──");
        ESP_LOGI(STATS_TAG, "  BLE RX throughput : %.2f KB/s",
                 ble_tput);
        ESP_LOGI(STATS_TAG, "  UART TX throughput: %.2f KB/s (theoretical max: %.2f KB/s)",
                 uart_tput, uart_baud_kbps);
        ESP_LOGI(STATS_TAG, "  Theoretical FPS   : %.2f (if streaming, incl. UART)",
                 theo_fps);
        ESP_LOGI(STATS_TAG, "  Free heap         : %lu bytes",
                 (unsigned long)esp_get_free_heap_size());
        ESP_LOGI(STATS_TAG, "%s", STATS_SEPARATOR);

        /* Reset reassembly state (buffer is kept for reuse) */
        s_img_reassembly_active = false;
        s_img_received_chunks   = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PHOTO REQUEST (TRIGGER)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Request a photo from the ESP-EYE by writing to the trigger characteristic.
 *
 * This function can be called from anywhere:
 *   - After initial notification enable (for testing)
 *   - From a cloud actuation handler (future integration)
 *   - From a button press handler, timer, etc.
 */
static void request_photo(void)
{
    if (!profile_tab[PROFILE_APP_ID].connected || s_trig_char_handle == 0) {
        ESP_LOGW(TAG, "Cannot request photo: connected=%d, trig_handle=%u",
                 profile_tab[PROFILE_APP_ID].connected, s_trig_char_handle);
        return;
    }

    uint8_t cmd = 0x01;  /* Trigger capture command */
    esp_err_t ret = esp_ble_gattc_write_char(
        profile_tab[PROFILE_APP_ID].gattc_if,
        profile_tab[PROFILE_APP_ID].conn_id,
        s_trig_char_handle,
        sizeof(cmd),
        &cmd,
        ESP_GATT_WRITE_TYPE_RSP,       /* Write with response */
        ESP_GATT_AUTH_REQ_NONE
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "request_photo write failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Photo request sent to ESP-EYE");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GAP EVENT HANDLER (BLE scanning & connection)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan param set failed, status=0x%x",
                     param->scan_param_cmpl.status);
            break;
        }
        ESP_LOGI(TAG, "Scan params set — starting scan (%d s)", SCAN_DURATION_SEC);
        esp_ble_gap_start_scanning(SCAN_DURATION_SEC);
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan start failed, status=0x%x",
                     param->scan_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = param;

        switch (scan_result->scan_rst.search_evt) {

        case ESP_GAP_SEARCH_INQ_RES_EVT: {
            /* Try to find the device name in the advertisement data */
            uint8_t *adv_name = NULL;
            uint8_t  adv_name_len = 0;

            /* Check complete name first */
            adv_name = esp_ble_resolve_adv_data(
                scan_result->scan_rst.ble_adv,
                ESP_BLE_AD_TYPE_NAME_CMPL,
                &adv_name_len);

            /* Fall back to short name */
            if (adv_name == NULL) {
                adv_name = esp_ble_resolve_adv_data(
                    scan_result->scan_rst.ble_adv,
                    ESP_BLE_AD_TYPE_NAME_SHORT,
                    &adv_name_len);
            }

            if (adv_name != NULL && adv_name_len > 0 &&
                adv_name_len == strlen(REMOTE_DEVICE_NAME) &&
                memcmp(adv_name, REMOTE_DEVICE_NAME, adv_name_len) == 0)
            {
                ESP_LOGI(TAG, "Found \"%s\" — stopping scan and connecting",
                         REMOTE_DEVICE_NAME);
                esp_ble_gap_stop_scanning();
                esp_ble_gattc_open(
                    profile_tab[PROFILE_APP_ID].gattc_if,
                    scan_result->scan_rst.bda,
                    scan_result->scan_rst.ble_addr_type,
                    true);
            }
            break;
        }

        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            ESP_LOGI(TAG, "Scan complete — restarting scan");
            esp_ble_gap_start_scanning(SCAN_DURATION_SEC);
            break;

        default:
            break;
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan stop failed, status=0x%x",
                     param->scan_stop_cmpl.status);
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

    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GATTC PROFILE EVENT HANDLER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param)
{
    switch (event) {

    /* ── Application registered — start scanning ───────────────────────── */
    case ESP_GATTC_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATTC reg failed, status=%d", param->reg.status);
            break;
        }
        ESP_LOGI(TAG, "GATTC registered — setting scan params");
        esp_ble_gap_set_scan_params(&ble_scan_params);
        break;

    /* ── Connected to server ───────────────────────────────────────────── */
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Open failed, status=%d", param->open.status);
            break;
        }
        ESP_LOGI(TAG, "Connected to ESP-EYE, conn_id=%d", param->open.conn_id);

        profile_tab[PROFILE_APP_ID].conn_id   = param->open.conn_id;
        profile_tab[PROFILE_APP_ID].connected = true;
        memcpy(profile_tab[PROFILE_APP_ID].remote_bda,
               param->open.remote_bda, sizeof(esp_bd_addr_t));

        /* Request MTU exchange for larger BLE payloads */
        esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
        break;

    /* ── MTU negotiated — search for service ───────────────────────────── */
    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "MTU config failed, status=%d", param->cfg_mtu.status);
        }
        s_mtu = param->cfg_mtu.mtu;
        ESP_LOGI(TAG, "MTU set to %u — searching for service 0x%04X",
                 s_mtu, SVC_UUID);

        esp_ble_gattc_search_service(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            &remote_filter_service_uuid);
        break;

    /* ── Service discovery result ──────────────────────────────────────── */
    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == SVC_UUID)
        {
            ESP_LOGI(TAG, "Service 0x%04X found (handles %d–%d)",
                     SVC_UUID,
                     param->search_res.start_handle,
                     param->search_res.end_handle);

            profile_tab[PROFILE_APP_ID].service_start_handle =
                param->search_res.start_handle;
            profile_tab[PROFILE_APP_ID].service_end_handle =
                param->search_res.end_handle;
            profile_tab[PROFILE_APP_ID].service_found = true;
        }
        break;

    /* ── Service search complete — discover characteristics ────────────── */
    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Service search failed, status=%d",
                     param->search_cmpl.status);
            break;
        }
        if (!profile_tab[PROFILE_APP_ID].service_found) {
            ESP_LOGE(TAG, "Service 0x%04X not found on server", SVC_UUID);
            break;
        }

        uint16_t svc_start = profile_tab[PROFILE_APP_ID].service_start_handle;
        uint16_t svc_end   = profile_tab[PROFILE_APP_ID].service_end_handle;

        /* ── Discover image characteristic (0xFF01) ───────────────────── */
        uint16_t count = 0;
        esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            ESP_GATT_DB_CHARACTERISTIC,
            svc_start, svc_end, 0, &count);

        if (status != ESP_GATT_OK || count == 0) {
            ESP_LOGE(TAG, "No characteristics found (status=%d, count=%d)",
                     status, count);
            break;
        }

        ESP_LOGI(TAG, "Found %d characteristics in service", count);

        esp_gattc_char_elem_t *char_elems =
            (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * count);
        if (!char_elems) {
            ESP_LOGE(TAG, "malloc failed for char_elems");
            break;
        }

        /* Find image char (0xFF01) */
        uint16_t img_count = count;
        status = esp_ble_gattc_get_char_by_uuid(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            svc_start, svc_end,
            remote_filter_img_char_uuid,
            char_elems,
            &img_count);

        if (status == ESP_GATT_OK && img_count > 0) {
            s_img_char_handle = char_elems[0].char_handle;
            s_img_char_found = true;
            ESP_LOGI(TAG, "Image char 0x%04X found, handle=%d",
                     CHAR_IMG_UUID, s_img_char_handle);
        } else {
            ESP_LOGE(TAG, "Image char 0x%04X not found", CHAR_IMG_UUID);
        }

        /* Find trigger char (0xFF02) */
        uint16_t trig_count = count;
        status = esp_ble_gattc_get_char_by_uuid(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            svc_start, svc_end,
            remote_filter_trig_char_uuid,
            char_elems,
            &trig_count);

        if (status == ESP_GATT_OK && trig_count > 0) {
            s_trig_char_handle = char_elems[0].char_handle;
            s_trig_char_found = true;
            ESP_LOGI(TAG, "Trigger char 0x%04X found, handle=%d",
                     CHAR_TRIG_UUID, s_trig_char_handle);
        } else {
            ESP_LOGE(TAG, "Trigger char 0x%04X not found", CHAR_TRIG_UUID);
        }

        free(char_elems);

        /* Register for notifications on the image characteristic */
        if (s_img_char_found) {
            esp_ble_gattc_register_for_notify(
                gattc_if,
                profile_tab[PROFILE_APP_ID].remote_bda,
                s_img_char_handle);
        }
        break;
    }

    /* ── Registered for notifications — write CCCD to enable ──────────── */
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Register for notify failed, status=%d",
                     param->reg_for_notify.status);
            break;
        }
        ESP_LOGI(TAG, "Registered for notify — writing CCCD to enable");

        /* Find the CCCD descriptor for the image characteristic */
        uint16_t descr_count = 0;
        esp_gatt_status_t ret_status = esp_ble_gattc_get_attr_count(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            ESP_GATT_DB_DESCRIPTOR,
            profile_tab[PROFILE_APP_ID].service_start_handle,
            profile_tab[PROFILE_APP_ID].service_end_handle,
            s_img_char_handle,
            &descr_count);

        if (ret_status != ESP_GATT_OK || descr_count == 0) {
            ESP_LOGE(TAG, "No descriptors found (status=%d, count=%d)",
                     ret_status, descr_count);
            break;
        }

        esp_gattc_descr_elem_t *descr_elems =
            (esp_gattc_descr_elem_t *)malloc(
                sizeof(esp_gattc_descr_elem_t) * descr_count);
        if (!descr_elems) {
            ESP_LOGE(TAG, "malloc failed for descr_elems");
            break;
        }

        ret_status = esp_ble_gattc_get_descr_by_char_handle(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            s_img_char_handle,
            notify_descr_uuid,
            descr_elems,
            &descr_count);

        if (ret_status != ESP_GATT_OK || descr_count == 0) {
            ESP_LOGE(TAG, "CCCD descriptor not found for image char");
            free(descr_elems);
            break;
        }

        /* Write 0x0001 to CCCD to enable notifications */
        uint16_t notify_en = 0x0001;
        esp_ble_gattc_write_char_descr(
            gattc_if,
            profile_tab[PROFILE_APP_ID].conn_id,
            descr_elems[0].handle,
            sizeof(notify_en),
            (uint8_t *)&notify_en,
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE);

        free(descr_elems);
        break;

    /* ── CCCD write complete — notifications now enabled ───────────────── */
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Write CCCD failed, status=%d", param->write.status);
            break;
        }
        ESP_LOGI(TAG, "Notifications enabled on ESP-EYE");

        /* Trigger an initial photo request for testing */
        ESP_LOGI(TAG, "Sending initial photo request...");
        request_photo();
        break;

    /* ── Characteristic write complete (trigger acknowledged) ──────────── */
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Write char failed, status=%d", param->write.status);
        } else {
            ESP_LOGI(TAG, "Trigger write acknowledged by ESP-EYE");
        }
        break;

    /* ── Notification received — image chunk data ──────────────────────── */
    case ESP_GATTC_NOTIFY_EVT:
        handle_image_chunk(param->notify.value, param->notify.value_len);
        break;

    /* ── Disconnected — reset state and restart scan ──────────────────── */
    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Disconnected from ESP-EYE, reason=0x%x — restarting scan",
                 param->disconnect.reason);

        /* Reset BLE state */
        profile_tab[PROFILE_APP_ID].connected     = false;
        profile_tab[PROFILE_APP_ID].service_found  = false;
        s_img_char_handle  = 0;
        s_trig_char_handle = 0;
        s_img_char_found   = false;
        s_trig_char_found  = false;

        /* Discard any partial image reassembly */
        if (s_img_reassembly_active) {
            ESP_LOGW(TAG, "Discarding partial frame due to disconnect");
            s_img_reassembly_active = false;
            s_img_received_chunks   = 0;
            xTimerStop(s_reassembly_timer, 0);
        }

        /* Restart scanning for ESP-EYE */
        esp_ble_gap_start_scanning(SCAN_DURATION_SEC);
        break;

    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GLOBAL GATTC DISPATCH
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Routes GATTC events to the correct profile handler.
 * On registration, remembers which interface belongs to our profile.
 */
static void gattc_event_handler(esp_gattc_cb_event_t event,
                                esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            profile_tab[PROFILE_APP_ID].gattc_if = gattc_if;
        } else {
            ESP_LOGE(TAG, "GATTC reg failed, app_id=%04x, status=%d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int i = 0; i < PROFILE_NUM; i++) {
        if (gattc_if == ESP_GATT_IF_NONE ||
            gattc_if == profile_tab[i].gattc_if)
        {
            if (profile_tab[i].gattc_cb) {
                profile_tab[i].gattc_cb(event, gattc_if, param);
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

    /* ── 2. Reassembly timeout timer (one-shot, 5 seconds) ────────────── */
    s_reassembly_timer = xTimerCreate(
        "reassembly_tmr",
        pdMS_TO_TICKS(REASSEMBLY_TIMEOUT_MS),
        pdFALSE,                          /* One-shot timer */
        NULL,
        reassembly_timeout_cb
    );
    if (!s_reassembly_timer) {
        ESP_LOGE(TAG, "Failed to create reassembly timer");
    }

    /* ── 3. Release Classic BT memory (ESP32-C6 is BLE-only) ──────────── */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    /* ── 4. Init and enable BT controller ─────────────────────────────── */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    /* ── 5. Init and enable Bluedroid host stack ──────────────────────── */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* ── 6. Register GATTC and GAP callbacks ──────────────────────────── */
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

    /* Register application profile — triggers ESP_GATTC_REG_EVT */
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(PROFILE_APP_ID));

    /* ── 7. Set local MTU for larger BLE payloads ─────────────────────── */
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(BLE_MTU_SIZE));

    /* ── 8. Start water sensor + servo task (runs independently) ──────── */
    xTaskCreate(water_sensor_task, "water_sensor", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "ESP32-C6 ready: BLE client + water sensor + console UART forwarding");
    ESP_LOGI(TAG, "Scanning for \"%s\"...", REMOTE_DEVICE_NAME);
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
}
