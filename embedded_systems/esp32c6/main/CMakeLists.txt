idf_component_register(SRCS "ota.c" "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_adc driver nvs_flash esp_wifi esp_netif esp_event esp_https_ota esp_http_client mqtt app_update espressif__cjson)