# Screen layouts by display shape and colour, and the input that drives them
# (touch or buttons, selected by the profile's input capability).
set(kbx_ui_srcs)
if(CONFIG_KBX_INPUT_BUTTONS)
    list(APPEND kbx_ui_srcs "buttons.c")
elseif(CONFIG_KBX_INPUT_TOUCH)
    list(APPEND kbx_ui_srcs "touch.c")
endif()
if(CONFIG_KBX_HOME_SCREEN)
    list(APPEND kbx_ui_srcs "ui_home.c"
        "assets/fonts/kbx_home_96.c" "assets/fonts/kbx_home_44.c")
endif()
if(CONFIG_KBX_DISPLAY_SHAPE_WIDE)
    list(APPEND kbx_ui_srcs "assets/fonts/kbx_lat_28.c")
endif()

idf_component_register(
    SRCS "ui_scene.c" "ui_overlays.c" "ui_style.c" "aura_orb.c" "display_profile.c" "talk_action.c"
         ${kbx_ui_srcs}
         "assets/kombify_rosette_104.c"
         "assets/fonts/kbx_lat_14.c" "assets/fonts/kbx_lat_16.c" "assets/fonts/kbx_lat_18.c" "assets/fonts/kbx_lat_20.c"
         "assets/fonts/kbx_zh_14.c" "assets/fonts/kbx_zh_16.c" "assets/fonts/kbx_zh_18.c" "assets/fonts/kbx_zh_20.c"
         "assets/fonts/kbx_ar_14.c" "assets/fonts/kbx_ar_16.c" "assets/fonts/kbx_ar_18.c" "assets/fonts/kbx_ar_20.c"
         "assets/fonts/kbx_num_56.c"
    INCLUDE_DIRS "include"
    REQUIRES kbx_core esp_driver_i2c
    PRIV_REQUIRES kbx_audio kbx_board kbx_net kbx_sensors kbx_voice kbx_wake
                  esp_driver_gpio esp_lcd esp_lcd_touch esp_lcd_touch_cst816s esp_timer lvgl
)
