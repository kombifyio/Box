/*
 * kombify box - edge device sync client with remote OTA.
 *
 * The box lists itself in the owner's homelab device list and is managed
 * remotely, strictly pull-only: it polls Gateway over HTTPS with the access
 * token of its existing Auth0 device-grant refresh token and holds no other
 * credential. Wire contract: the kombify Connect contract, section 9.2 (edge
 * devices).
 *
 *   PUT  <base>/v1/connect/edge-devices/<device_id>            check-in
 *   GET  <base>/v1/connect/edge-devices/<device_id>/settings   If-None-Match
 *   PUT  <base>/v1/connect/edge-devices/<device_id>/settings   If-Match
 *
 * device_id = "dev_" + unpadded base64url HMAC-SHA256 of
 * "kombify-connect-device:v1:<sub>" keyed with the factory base MAC; sub is
 * read from the payload of the box's own access token.
 *
 * Cadence: check-in on boot/Wi-Fi up, when the reported state changed (at
 * most every 30 s) and every 5 min; settings poll about every 60 s. Sync is
 * off while a voice conversation, a TLS handshake of the voice path or any
 * firmware transfer runs, so it never competes with voice for heap or TLS.
 * Account documents apply through device_config; local changes (Quick
 * Settings, KBX, debounced volume) are written back with If-Match.
 *
 * Remote OTA: firmware.update_request above the persisted handled counter
 * triggers one check + install; firmware.auto_update checks about every 6 h
 * and installs only while idle; firmware.target_version pins one release
 * (downgrade allowed). The manifest verification in fw_ota stays the only
 * image authority.
 *
 * Gateway base: https://api.kombify.io. Development bench builds
 * (CONFIG_KBX_DEV_ENDPOINTS) add a persisted override limited to exactly
 * https://api.kombify.io or the Preview Gateway (KBX sync base ...); a
 * release image holds no Preview host.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_SYNC_BASE_DEFAULT "https://api.kombify.io"
#if CONFIG_KBX_DEV_ENDPOINTS
#define EDGE_SYNC_BASE_PREVIEW "https://gateway.preview.invalid"
#endif

/* After device_config_init and cloud_auth_init: starts the sync task. */
esp_err_t edge_sync_init(void);

/* Gateway base in use (the default, or Preview on a bench build). */
const char *edge_sync_gateway_base(void);
/* Persist the base: only the allowed origins; NULL, "" or "default" resets. */
esp_err_t edge_sync_set_gateway(const char *url);
bool edge_sync_gateway_allowed(const char *url);

/* Check in and poll as soon as the box is quiet. */
void edge_sync_poke(void);

void edge_sync_log_status(void);

/* Edge device id (dev_...) this box checks in as, known after its first cycle
 * and kept across reboots; NULL before the first pairing cycle. */
const char *edge_sync_device_id(void);

/* KBX console: "sync?", "sync now", "sync body", "sync base [?|default|<url>]";
 * bench builds add "sync base preview" and "sync put".
 * Returns true when the line was a sync command. */
bool edge_sync_console(const char *line);

#ifdef __cplusplus
}
#endif
