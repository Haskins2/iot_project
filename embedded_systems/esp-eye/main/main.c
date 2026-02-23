#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "esp_eye";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-EYE boilerplate started");

    while (1)
    {
        ESP_LOGI(TAG, "Heartbeat");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
