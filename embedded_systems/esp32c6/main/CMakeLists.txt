idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_adc driver nvs_flash esp_wifi esp_netif esp_event mqtt esp_timer esp_https_ota esp_http_client app_update espressif__cjson)