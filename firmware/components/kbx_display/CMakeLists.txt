# Panel drivers, selected by the profile's display capability.
set(kbx_display_srcs)
if(CONFIG_KBX_DISPLAY_ST7305_RLCD)
    list(APPEND kbx_display_srcs "display_rlcd.c")
elseif(CONFIG_KBX_DISPLAY_ST77916_QSPI)
    list(APPEND kbx_display_srcs "display.c")
endif()

idf_component_register(
    SRCS ${kbx_display_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES kbx_board kbx_ui esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi esp_timer
                  esp_lcd esp_lcd_st77916 esp_io_expander esp_io_expander_tca9554 esp_lvgl_port lvgl
)
