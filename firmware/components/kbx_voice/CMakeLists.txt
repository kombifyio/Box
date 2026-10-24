# Voice session client: the SpeechKit voice protocol over one WebSocket.
idf_component_register(
    SRCS "net_ws.c" "session_url.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
    PRIV_REQUIRES kbx_audio kbx_core kbx_net kbx_wake esp_websocket_client esp_http_client esp-tls mbedtls
                  esp_timer nvs_flash
)

# Re-apply the PSRAM-stack overlay onto the managed websocket client. The
# Component Manager restores the upstream file; largest internal heap after
# LVGL/WiFi/BT is ~3.5 KB so the 10 KB websocket task cannot start otherwise.
idf_build_get_property(kbx_project_dir PROJECT_DIR)
set(_kbx_ws_overlay "${kbx_project_dir}/overlays/esp_websocket_client.c")
set(_kbx_ws_dst "${kbx_project_dir}/managed_components/espressif__esp_websocket_client/esp_websocket_client.c")
if(EXISTS "${_kbx_ws_overlay}" AND EXISTS "${_kbx_ws_dst}")
    configure_file("${_kbx_ws_overlay}" "${_kbx_ws_dst}" COPYONLY)
endif()
