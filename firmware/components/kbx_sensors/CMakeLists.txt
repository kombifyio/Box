# Battery gauge and room climate sensor.
idf_component_register(
    SRCS "battery.c" "env_sensor.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES kbx_board esp_adc esp_driver_i2c nvs_flash
)
