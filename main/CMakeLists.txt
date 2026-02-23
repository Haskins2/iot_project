idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_adc esp_driver_ledc nvs_flash esp_wifi esp_netif esp_event)
