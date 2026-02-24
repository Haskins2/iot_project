# old UART cmake
# idf_component_register(SRCS "main.c"
#                        INCLUDE_DIRS "."
#                        PRIV_REQUIRES esp_adc esp_driver_ledc)

# new BT cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash bt)