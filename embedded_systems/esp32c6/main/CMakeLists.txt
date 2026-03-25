idf_component_register(
    SRCS
        "main.c"
        "raindrop_sensor.c"
        "water_sensor.c"
        "servo.c"
        "ble_client.c"
        "sensor_packet.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_adc
        nvs_flash
        bt
        esp_timer
)
