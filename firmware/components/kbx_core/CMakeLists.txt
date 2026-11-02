# Shared base: device settings, i18n, firmware update, cloud sign-in, edge
# sync, the LAN console and the panic record.
idf_component_register(
    SRCS "cloud_auth.c" "device_config.c" "display_design.c" "edge_sync.c" "fw_ota.c" "fw_update_manifest.c"
         "i18n.c" "i18n_table.c" "lan_console.c" "box_status.c" "secure_zero.c" "panic_record.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES kbx_audio kbx_board kbx_display kbx_net kbx_voice kbx_wake
                  espressif__cjson esp_app_format app_update esp_http_client esp_http_server esp_https_ota
                  esp-tls mbedtls esp_timer nvs_flash tinyusb
)

# panic_record.c: keep the last panic in RTC memory (USB CDC console).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
