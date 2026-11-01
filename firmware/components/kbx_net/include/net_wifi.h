/*
 * kombify box - Wi-Fi station with NVS credentials (track B5).
 *
 * Provisioning for bring-up uses the existing CDC channel:
 *   KBX wifi <ssid>|<password>   store credentials and connect
 *   KBX wifi?                    report current status into the log
 * Without stored credentials the module stays passive — the box is then a
 * pure USB device as before.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Initialize NVS/netif/Wi-Fi and connect if credentials are in NVS.
 * Without credentials: no Wi-Fi start, ESP_OK. */
esp_err_t net_wifi_init(void);

/* Persist credentials in NVS and (re)connect immediately. */
esp_err_t net_wifi_set_credentials(const char *ssid, const char *password);

/* Clear credentials and stop Wi-Fi. */
esp_err_t net_wifi_clear_credentials(void);

/* true once an IP has been obtained. ip_out (optional, >=16 bytes)
 * receives the IPv4 address as a string. */
bool net_wifi_is_connected(char *ip_out, size_t ip_out_len);

/* Wi-Fi power save for a voice turn: on = no modem sleep (the uplink socket
 * needs ACKs within tens of ms, not the ~110 ms of DTIM sleep), off = the
 * default modem-sleep mode. Ignored while Bluetooth is on (coexistence needs
 * power save). */
void net_wifi_set_low_latency(bool on);

/* Configured SSID (empty while no credentials are set). Shows what the box
 * is connected to or trying to connect to. */
const char *net_wifi_ssid(void);

/* Toggle the Wi-Fi radio WITHOUT clearing NVS credentials (unlike
 * net_wifi_clear_credentials). Off: disconnect + stop STA + halt reconnect
 * backoff. On: restart STA (connects with stored credentials if present).
 * For the UI switch. */
esp_err_t net_wifi_set_enabled(bool enabled);
bool net_wifi_is_enabled(void);

/* Log current status (SSID configured? connected? IP?). */
void net_wifi_log_status(void);

/* Stop + deinit the wifi driver so OTA SHA has internal RAM. Credentials stay. */
void net_wifi_release_ram(void);
