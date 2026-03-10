// captures and streams jpeg over UART (to python script)

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "driver/uart.h"

// Camera GPIO pins — ESP-EYE (ESP32-D0WD)
#define CAM_PIN_PWDN    -1  // Not used
#define CAM_PIN_RESET   -1  // Not used (software reset)
#define CAM_PIN_XCLK     4
#define CAM_PIN_SIOD    18  // SCCB data
#define CAM_PIN_SIOC    23  // SCCB clock
#define CAM_PIN_D7      36
#define CAM_PIN_D6      37
#define CAM_PIN_D5      38
#define CAM_PIN_D4      39
#define CAM_PIN_D3      35
#define CAM_PIN_D2      14
#define CAM_PIN_D1      13
#define CAM_PIN_D0      34
#define CAM_PIN_VSYNC    5
#define CAM_PIN_HREF    27
#define CAM_PIN_PCLK    25

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

    .xclk_freq_hz = 20000000,        // 20 MHz, standard for OV2640
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_QVGA,  // 320x240 (better suited for BLE)
    .jpeg_quality = 20,              // 0–63, lower = better quality
    .fb_count     = 1,               // 1 frame fits in DRAM
    .fb_location  = CAMERA_FB_IN_DRAM, // PSRAM not needed for QVGA
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

// UART functions

/* Initialise UART0 for raw binary output.
 * TX buf = 0: uart_write_bytes() blocks until FIFO is drained (no data loss).
 * printf/fwrite must not be used after this — they conflict with the driver. */
static void uart_init(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate  = 921600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_driver_install(UART_NUM_0, /*rx_buf=*/256, /*tx_buf=*/0, 0, NULL, 0);
}

static void uart_send(const char *msg)
{
    uart_write_bytes(UART_NUM_0, msg, strlen(msg));
}

// --- Transport & Packaging ---

typedef struct __attribute__((packed)) {
    uint16_t magic_header; // e.g. 0xFECA
    uint32_t total_len;    // Length of the full image frame
    uint16_t chunk_id;     // Optional: Future use for chunking
    uint16_t total_chunks; // Optional: Future use for chunking
} frame_header_t;

/* Abstract transport interface to easily switch from UART to BLE later */
static void transport_send_data(const uint8_t *data, size_t len)
{
    // For now, this just wraps UART. Later, this is where BLE notify/indicate will go.
    // If chunking logic is needed inside this method, split 'data' into chunks
    // and limit by BLE MTU before transmitting.
    uart_write_bytes(UART_NUM_0, data, len);
}

// Camera functions

static esp_err_t camera_init(void)
{
    return esp_camera_init(&CAMERA_CONFIG);
}

static void send_frame(const camera_fb_t *fb)
{
    frame_header_t header = {
        .magic_header = 0xFECA,
        .total_len    = fb->len,
        .chunk_id     = 0,     // For future: chunking support
        .total_chunks = 1      // For future: chunking support
    };

    // Send the binary metadata header
    transport_send_data((const uint8_t *)&header, sizeof(header));
    
    // Send payload (Future: slice this buffer using BLE_MTU here, or within transport_send_data)
    transport_send_data(fb->buf, fb->len);
}




void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE); // silence logs — they corrupt the UART stream (Just temporary as we use UART)
    uart_init();

    esp_err_t err = camera_init();
    if (err != ESP_OK) {
        char msg[32];
        snprintf(msg, sizeof(msg), "CAM_ERR:0x%x\n", err);
        uart_send(msg);
        return;
    }

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            uart_send("CAM_ERR:capture_failed\n");
            continue;
        }
        send_frame(fb);
        esp_camera_fb_return(fb);
    }
}
