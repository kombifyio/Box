# Application: boot sequence, USB console with host link, I2C diagnostics.
# Capability modules live in components/kbx_*; the board profile in
# Kconfig.projbuild selects their sources.
idf_component_register(
    SRCS "kombify_main.c" "usb_log.c" "diag_i2c.c"
    INCLUDE_DIRS "."
    REQUIRES kbx_audio kbx_board kbx_core kbx_display kbx_net kbx_sensors kbx_ui kbx_voice kbx_wake
             esp_driver_gpio esp_driver_i2c esp_io_expander_tca9554 espressif__cjson espcoredump tinyusb
)
