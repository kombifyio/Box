/*
 * kombify box - USB voice satellite for SpeechKit
 * Waveshare ESP32-S3-Touch-LCD-1.85C or ESP32-S3-RLCD-4.2 (board.h)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "panic_record.h"
#include "cJSON.h"
#include "board.h"
#include "audio.h"
#include "cloud_auth.h"
#include "bt_radio.h"
#include "display.h"
#include "fw_ota.h"
#include "battery.h"
#include "stop_word.h"
#include "env_sensor.h"
#include "i18n.h"
#include "display_design.h"
#include "lan_console.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "touch.h"
#include "usb_log.h"
#include "device_config.h"
#include "edge_sync.h"
#include "wakeword.h"

static const char *TAG = "kombify_box";

static void *json_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void app_main(void)
{
#if CONFIG_KBX_POWER_LATCH
    /* keep board powered when running from battery */
    gpio_reset_pin(PWR_CONTROL_GPIO);
    gpio_set_direction(PWR_CONTROL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PWR_CONTROL_GPIO, 1);
#endif

    /* Release BT-controller RAM immediately (before large allocations) so
     * internal RAM is enough for net_ws + the second TLS handshake (wss).
     * Disables the BLE tile — deliberate M1 trade (voice > BLE). */
    bt_radio_release();

    ESP_LOGI(TAG, "kombify box starting (reset %s)", fw_ota_reset_reason_name());
    /* All JSON users share one allocator, installed before any task parses
     * a response. OAuth payloads must not consume the scarce DMA/Wi-Fi heap. */
    cJSON_Hooks json_hooks = { .malloc_fn = json_alloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&json_hooks);
    i18n_init();   /* locale before the first label is built */
    kbx_design_init(KBX_DESIGN_CLASSIC);   /* the display design before the first widget; account setting display.design */
    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "display init failed - continuing, USB audio stays available");
    }
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed - continuing without mute button");
    }
    /* Wi-Fi before audio: I2S DMA lives in internal RAM and otherwise leaves
     * esp_wifi_init() with no 30 KB block (ESP_ERR_NO_MEM). */
    usb_log_install();
    /* Repeated once the CDC log exists: the cause of the previous reset is
     * the first thing to read after a crash (also in KBX version?). */
    ESP_LOGI(TAG, "boot: reset=%s%s", fw_ota_reset_reason_name(),
             fw_ota_reset_was_crash() ? " (crash)" : "");
    if (panic_record_valid()) {
        panic_record_log();   /* stays until KBX coredump clear */
    }
    if (esp_core_dump_image_check() == ESP_OK) {
        ESP_LOGW(TAG, "boot: a core dump is stored - KBX coredump? shows it, KBX coredump clear erases it");
    }
    if (net_wifi_init() != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed - continuing as USB-only device");
    }
    ESP_ERROR_CHECK(audio_init());
#if CONFIG_KBX_STOP_WORD
    /* "Stop" while a reply plays; needs the echo canceller (audio_init). */
    if (stop_word_init() != ESP_OK) {
        ESP_LOGW(TAG, "stop word unavailable");
    }
#endif
#if CONFIG_KBX_BATTERY
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge unavailable");
    }
#endif
#if CONFIG_KBX_ENV_SENSOR
    /* Room climate for the home screen (SHTC3 on the system I2C bus). */
    if (env_sensor_init() != ESP_OK) {
        ESP_LOGW(TAG, "room climate sensor unavailable");
    }
#endif
    /* LAN console: off unless the owner stored a key over USB. */
    static const lan_console_commands_t lan_commands = {
        .capture_begin = usb_log_capture_begin,
        .exec_line = usb_log_exec_line,
        .capture_end = usb_log_capture_end,
    };
    if (lan_console_init(&lan_commands) != ESP_OK) {
        ESP_LOGW(TAG, "LAN console unavailable - USB control remains");
    }
    if (cloud_auth_init() != ESP_OK) {
        ESP_LOGE(TAG, "cloud pairing initialization failed");
    }
    /* net_ws: standalone voice-agent client. Lazy — dials only after
     * provisioning (KBX server/token) + "KBX agent on" + Wi-Fi. */
    if (net_ws_init() != ESP_OK) {
        ESP_LOGW(TAG, "net_ws init failed - standalone voice unavailable");
    }
    /* Local wake word: tap-only when the model cannot load. */
    if (wakeword_init() != ESP_OK) {
        ESP_LOGW(TAG, "local wake word unavailable - tap to talk remains available");
    }
    /* Account device settings (wake word from Companion Studio): sync state. */
    if (device_config_init() != ESP_OK) {
        ESP_LOGW(TAG, "device settings sync unavailable - local settings keep working");
    }
    /* Edge device sync: appears in the homelab device list and applies the
     * account's settings and firmware policy; pull-only, own task. */
    if (edge_sync_init() != ESP_OK) {
        ESP_LOGW(TAG, "edge device sync unavailable - local settings keep working");
    }
    bt_radio_init();   /* no-op; BT RAM released above, tile stays off */
    audio_play_ready_chime();

    /* Mark PENDING_VERIFY valid here: app_main only reaches this point
     * after display+audio. Waiting for USB or a Wi-Fi IP restarted a
     * healthy speaker every 60 s whenever no PC was plugged in. */
    (void)fw_ota_init();

    ESP_LOGI(TAG, "ready - visible to host as USB audio device 'kombify box'");
}
