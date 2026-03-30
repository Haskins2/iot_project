#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

#define VERSION_URL             "https://github.com/Haskins2/iot_project/releases/latest/download/version.json"
#define FIRMWARE_URL            "https://github.com/Haskins2/iot_project/releases/latest/download/esp32c6.bin"

#define CURRENT_VERSION         "1.1.0"

#define OTA_CHECK_INTERVAL_MS   (24 * 60 * 60 * 1000)

static const char *TAG = "OTA";

static esp_err_t _version_http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (!esp_http_client_is_chunked_response(evt->client)) {
            char *buf = (char *)evt->user_data;
            if (strlen(buf) + evt->data_len < 255) {
                strncat(buf, (char *)evt->data, evt->data_len);
            }
        }
    }
    return ESP_OK;
}

static int compare_versions(const char *v1, const char *v2) {
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

static bool ota_check_new_version_available(void) {
    char response_buf[256] = {0};

    esp_http_client_config_t version_config = {
        .url                   = VERSION_URL,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .max_redirection_count = 5,
        .buffer_size           = 8192,
        .buffer_size_tx        = 2048,
        .event_handler         = _version_http_event_handler,
        .user_data             = response_buf,
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
    ESP_LOGI(TAG, "HTTP status: %d", status);

    if (status != 200 && status != 301 && status != 302) {
        ESP_LOGE(TAG, "HTTP error status: %d", status);
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI(TAG, "Read %d bytes. Version response payload: '%s'", (int)strlen(response_buf), response_buf);
    
    esp_http_client_cleanup(client);

    if (strlen(response_buf) == 0) {
        return false;
    }

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

    bool update_available = compare_versions(version_item->valuestring, CURRENT_VERSION) > 0;
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

    if (ota_check_new_version_available()) {
        esp_http_client_config_t ota_config = {
            .url               = FIRMWARE_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
            .buffer_size       = 8192,
            .buffer_size_tx    = 2048,
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