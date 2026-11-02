# Board profile: pins and numeric tuning per board (board.h is the only header
# that selects by board identity), the shared system I2C bus on those pins and
# the hardware probe that names the board independent of the image.
idf_component_register(
    SRCS "system_i2c.c" "board_identity.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c
    PRIV_REQUIRES nvs_flash
)
