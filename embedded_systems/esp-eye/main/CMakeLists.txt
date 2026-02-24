# old UART cmake
# idf_component_register(SRCS "main.c"
#                        INCLUDE_DIRS "."
#                        PRIV_REQUIRES esp_driver_uart)

# new BT cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash bt)