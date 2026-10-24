/*
 * kombify box - field firmware update over CDC or HTTP, plus rollback health.
 *
 * USB download-mode flash remains the recovery path. After one image that
 * contains this module is on the device, later app updates go through
 * KBX ota-begin (CDC) or the WLAN update checker without holding BOOT.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mark a PENDING_VERIFY slot valid once display+audio have come up.
 * No-op when the running slot is already valid. */
esp_err_t fw_ota_init(void);

void fw_ota_log_version(void);
void fw_ota_log_status(void);

/* Why this boot happened (esp_reset_reason), as a short log name. */
const char *fw_ota_reset_reason_name(void);
/* True when the last reset was a crash: panic, any watchdog or brownout. */
bool fw_ota_reset_was_crash(void);

typedef enum {
    FW_OTA_UPDATE_IDLE = 0,
    FW_OTA_UPDATE_CHECKING,
    FW_OTA_UPDATE_CURRENT,
    FW_OTA_UPDATE_AVAILABLE,
    FW_OTA_UPDATE_INSTALLING,
    FW_OTA_UPDATE_REBOOTING,
    FW_OTA_UPDATE_ERROR,
} fw_ota_update_state_t;

typedef struct {
    fw_ota_update_state_t state;
    char version[32];
    char detail[96];
    unsigned progress_percent;
} fw_ota_update_status_t;

/* Async WLAN update, safe to call from LVGL. Install uses only the manifest
 * accepted by the last successful check; no caller-supplied image authority. */
esp_err_t fw_ota_check_update(void);
/* Same, for one release ("0.31.0", downgrade allowed): fetches that
 * release's manifest, which must name exactly this version. NULL or empty
 * checks the latest release, like fw_ota_check_update(). */
esp_err_t fw_ota_check_update_version(const char *version);
/* A check, install, CDC or LAN transfer is running or the box is rebooting
 * into a new image; remote sync and other background work stay off. */
bool fw_ota_is_busy(void);
esp_err_t fw_ota_install_update(void);
void fw_ota_get_update_status(fw_ota_update_status_t *out);

/* Compatibility CDC command: accepts only the exact HTTPS image URL from
 * the last validated manifest. Call fw_ota_check_update() before using it. */
esp_err_t fw_ota_start_http(const char *url);

/* Prepare the inactive OTA slot and switch the CDC RX path to raw binary.
 * Host must wait for the "kbx_ota: recv" log line before sending image_size
 * bytes. sha256_hex is optional (64 hex chars); when set, the written image
 * is checked before the boot partition is switched. */
esp_err_t fw_ota_begin_cdc(size_t image_size, const char *sha256_hex);

/* LAN push (lan_console.c): the caller streams the image from its own
 * task, which must run on an internal-RAM stack (flash writes unmap PSRAM
 * stacks). want_sha256 is required; finish checks it before switching the
 * boot slot and then reboots. Wi-Fi stays up, voice is paused. */
esp_err_t fw_ota_stream_begin(size_t image_size, const uint8_t want_sha256[32]);
esp_err_t fw_ota_stream_write(const void *data, size_t len);
esp_err_t fw_ota_stream_finish(void);
void fw_ota_stream_abort(const char *why);
/* After a successful finish and the HTTP reply: restart into the new slot. */
void fw_ota_stream_reboot(void);

/* True while the CDC parser must not interpret incoming bytes as lines. */
bool fw_ota_cdc_active(void);

/* Reboot into the ROM serial download loader (PID 1001) without BOOT. */
void fw_ota_reboot_to_download(void);

/* Restart the selected image without changing firmware or stored settings. */
void fw_ota_reboot(void);

#ifdef __cplusplus
}
#endif
