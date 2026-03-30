#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

#define VERSION_URL             "https://github.com/Haskins2/iot_project/releases/latest/download/version.json"
#define FIRMWARE_URL            "https://github.com/Haskins2/iot_project/releases/latest/download/esp32c6.bin"

#ifndef CURRENT_VERSION
#define CURRENT_VERSION         "0.0.0"
#endif

#define OTA_CHECK_INTERVAL_MS   (24 * 60 * 60 * 1000)

static const char *TAG = "OTA";

static char github_pem_buf[4096];

static esp_err_t load_github_pem_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("certs", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace 'certs': %s", esp_err_to_name(err));
        return err;
    }

    size_t pem_len = sizeof(github_pem_buf);
    err = nvs_get_blob(handle, "github_pem", github_pem_buf, &pem_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Missing key: github_pem -> %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    ESP_LOGI(TAG, "GitHub PEM loaded from NVS (%d bytes)", pem_len);
    nvs_close(handle);
    return ESP_OK;
}

static bool ota_check_new_version_available(void) {
    char response_buf[256] = {0};

    esp_http_client_config_t version_config = {
        .url                   = VERSION_URL,
        .cert_pem              = github_pem_buf,
        .max_redirection_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&version_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    int read_len = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP status: %d, content length: %d", status, read_len);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error status: %d", status);
        esp_http_client_cleanup(client);
        return false;
    }

    read_len = esp_http_client_read_response(client, response_buf, sizeof(response_buf) - 1);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        ESP_LOGE(TAG, "Failed to read version response");
        return false;
    }

    ESP_LOGI(TAG, "Version response: %s", response_buf);

    cJSON *json = cJSON_Parse(response_buf);
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse version JSON");
        return false;
    }

    cJSON *version_item = cJSON_GetObjectItem(json, "version");
    if (!cJSON_IsString(version_item)) {
        ESP_LOGE(TAG, "No version field in JSON");
        cJSON_Delete(json);
        return false;
    }

    bool update_available = strcmp(version_item->valuestring, CURRENT_VERSION) != 0;
    if (update_available) {
        ESP_LOGI(TAG, "New version available: %s (current: %s)",
                 version_item->valuestring, CURRENT_VERSION);
    } else {
        ESP_LOGI(TAG, "Firmware is up to date: %s", CURRENT_VERSION);
    }

    cJSON_Delete(json);
    return update_available;
}

static void check_and_update(void) {
    ESP_LOGI(TAG, "Checking for firmware update...");

    if (load_github_pem_from_nvs() != ESP_OK) {
        ESP_LOGE(TAG, "Cannot perform OTA without GitHub PEM");
        return;
    }

    if (ota_check_new_version_available()) {
        esp_http_client_config_t ota_config = {
            .url               = FIRMWARE_URL,
            .cert_pem          = github_pem_buf,
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
    }
}

void ota_task(void *pvParameter) {
    check_and_update();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
        check_and_update();
    }
}

void ota_mark_valid(void) {
    esp_ota_mark_app_valid_cancel_rollback();
}