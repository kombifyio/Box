# Radios: Wi-Fi station with SNTP, and the Bluetooth controller switch.
idf_component_register(
    SRCS "net_wifi.c" "bt_radio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash bt
)
