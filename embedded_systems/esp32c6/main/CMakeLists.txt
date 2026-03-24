idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_adc esp_driver_ledc bt nvs_flash mbedtls)
