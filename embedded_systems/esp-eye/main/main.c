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
    .frame_size   = FRAMESIZE_UXGA,  // 1600×1200
    .jpeg_quality = 10,              // 0–63, lower = better quality
    .fb_count     = 2,
    .fb_location  = CAMERA_FB_IN_PSRAM,
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

// Camera functions

static esp_err_t camera_init(void)
{
    return esp_camera_init(&CAMERA_CONFIG);
}

static void send_frame(const camera_fb_t *fb)
{
    char header[32];
    int n = snprintf(header, sizeof(header), "FRAME:%zu\n", fb->len);
    uart_write_bytes(UART_NUM_0, header, n);
    uart_write_bytes(UART_NUM_0, fb->buf, fb->len);
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
