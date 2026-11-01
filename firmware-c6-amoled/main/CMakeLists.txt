# Board-independent modules come straight from the S3 Box firmware so both
# variants keep one implementation of Cloud pairing, Wi-Fi, the Gateway URL
# rules, the voice protocol, the wake word engine, the theme and the brand
# assets.
set(kbx_shared "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components")

idf_component_register(
    SRCS "c6_main.c" "c6_audio.c" "c6_voice.c" "c6_ui.c" "c6_console.c" "c6_wake.c" "c6_power.c" "c6_edge.c" "c6_sd.c"
         "${kbx_shared}/kbx_core/cloud_auth.c"
         "${kbx_shared}/kbx_core/display_design.c"
         "${kbx_shared}/kbx_net/net_wifi.c"
         "${kbx_shared}/kbx_voice/session_url.c"
         "${kbx_shared}/kbx_wake/wakeword_mww.cc"
         "${kbx_shared}/kbx_ui/assets/kombify_rosette_104.c"
         "${kbx_shared}/kbx_ui/assets/kombify_rosette_186.c"
         "${kbx_shared}/kbx_ui/assets/kbx_orb_glow.c"
         "${kbx_shared}/kbx_ui/assets/fonts/kbx_lat_16.c"
         "${kbx_shared}/kbx_ui/assets/fonts/kbx_lat_20.c"
         "${kbx_shared}/kbx_ui/assets/fonts/kbx_lat_28.c"
    INCLUDE_DIRS "." "${kbx_shared}/kbx_core/include" "${kbx_shared}/kbx_net/include"
                 "${kbx_shared}/kbx_voice" "${kbx_shared}/kbx_voice/include" "${kbx_shared}/kbx_wake"
                 "${kbx_shared}/kbx_ui"
    REQUIRES esp_driver_gpio fatfs vfs esp_driver_i2c esp_driver_i2s esp_driver_usb_serial_jtag esp_ringbuf esp_timer
             esp_wifi esp_netif esp_event nvs_flash app_update esp_app_format esp_http_client esp-tls mbedtls
             esp_websocket_client tcp_transport espressif__cjson esp_lvgl_port lvgl esp32_c6_touch_amoled_1_8
             espressif__esp-tflite-micro esphome__esp-micro-speech-features
)

# No PSRAM: the control-message reassembly buffer is 8 KiB instead of 64 KiB.
target_compile_definitions(${COMPONENT_LIB} PRIVATE KBX_VOICE_TEXT_LIMIT=8192U
    # The token only mints a session; renew it 90 s before expiry instead of
    # 10 min, so a 15-minute token is not renewed on nearly every turn.
    CLOUD_AUTH_RENEW_MARGIN_S=90)

# Bench builds only: another Gateway origin for the edge-device sync
# (idf.py -DC6_EDGE_BASE=https://gateway.preview.invalid build).
if(C6_EDGE_BASE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C6_EDGE_BASE="${C6_EDGE_BASE}")
endif()

# microWakeWord models (c6_wake.c), the same flatbuffers as the S3 Box:
# kombify, hey_kombify and hey_kubi from wakeword-training, hey_jarvis from
# esphome/micro-wake-word-models v2 (Apache-2.0). Each on a 16-byte boundary.
set(kbx_mww_dir "${kbx_shared}/../models/mww")
set(kbx_mww_files "${kbx_mww_dir}/kombify.tflite" "${kbx_mww_dir}/hey_kombify.tflite"
    "${kbx_mww_dir}/hey_kubi.tflite" "${kbx_mww_dir}/hey_jarvis.tflite")
set(kbx_mww_asm "${CMAKE_CURRENT_BINARY_DIR}/kbx_mww_models.S")
file(CONFIGURE OUTPUT "${kbx_mww_asm}" CONTENT [=[
    .section .rodata.kbx_mww, "a"
    .balign 16
    .global kbx_mww_kombify_start
kbx_mww_kombify_start:
    .incbin "${kbx_mww_dir}/kombify.tflite"
    .global kbx_mww_kombify_end
kbx_mww_kombify_end:
    .balign 16
    .global kbx_mww_hey_kombify_start
kbx_mww_hey_kombify_start:
    .incbin "${kbx_mww_dir}/hey_kombify.tflite"
    .global kbx_mww_hey_kombify_end
kbx_mww_hey_kombify_end:
    .balign 16
    .global kbx_mww_hey_kubi_start
kbx_mww_hey_kubi_start:
    .incbin "${kbx_mww_dir}/hey_kubi.tflite"
    .global kbx_mww_hey_kubi_end
kbx_mww_hey_kubi_end:
    .balign 16
    .global kbx_mww_hey_jarvis_start
kbx_mww_hey_jarvis_start:
    .incbin "${kbx_mww_dir}/hey_jarvis.tflite"
    .global kbx_mww_hey_jarvis_end
kbx_mww_hey_jarvis_end:
]=])
target_sources(${COMPONENT_LIB} PRIVATE "${kbx_mww_asm}")
set_source_files_properties("${kbx_mww_asm}" PROPERTIES OBJECT_DEPENDS "${kbx_mww_files}")
