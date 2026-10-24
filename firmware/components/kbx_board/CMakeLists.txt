# Board profile: pins and numeric tuning per board (board.h is the only header
# that selects by board identity) and the shared system I2C bus on those pins.
idf_component_register(
    SRCS "system_i2c.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c
)
