#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "cJSON.h"

#define VERSION_URL "https://github.com/Haskins2/iot_project/releases/latest/download/version.json"
#define FIRMWARE_URL "https://github.com/Haskins2/iot_project/releases/latest/download/esp32c6.bin"
#define CURRENT_VERSION "1.0.0"

extern const char github_pem_start[] asm("_binary_github_pem_start");
extern const char github_pem_end[]   asm("_binary_github_pem_end");

static const char *TAG = "OTA";

void ota_task(void *pvParameter) {
    // 1. Check version.json first
    esp_http_client_config_t version_config = {
        .url = VERSION_URL,
        .cert_pem = github_pem_start,
    };
    // ... fetch and parse version.json, compare with CURRENT_VERSION
    // ... if same version, skip update

    // 2. Perform OTA update
    esp_http_client_config_t ota_config = {
        .url = FIRMWARE_URL,
        .cert_pem = github_pem_start,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t config = {
        .http_config = &ota_config,
    };

    ESP_LOGI(TAG, "Starting OTA update...");
    esp_err_t ret = esp_https_ota(&config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, rebooting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    vTaskDelete(NULL);
}

void ota_mark_valid(void) {
    esp_ota_mark_app_valid_cancel_rollback();
}