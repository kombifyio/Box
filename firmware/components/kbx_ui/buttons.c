/*
 * kombify box - button input for boards without touch (Waveshare RLCD 4.2).
 *
 * Implements the touch.h contract so the rest of the firmware is unchanged:
 *   KEY  short press -> talk (same action as an orb tap)
 *   KEY  hold 800 ms -> local mute latch
 *   BOOT short press -> volume +8, from 100 back to 20
 *   BOOT hold 800 ms -> account pairing sheet with the QR code (starts
 *                       pairing when unpaired); held again it closes
 * Both keys are active low with 10K pull-ups on the board; polled every
 * 20 ms, which is also the debounce. The child lock is kept for the
 * settings panel but has no unlock gesture without touch, so the board
 * ignores it.
 */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "board.h"
#include "audio.h"
#include "cloud_auth.h"
#include "device_config.h"
#include "net_ws.h"
#include "talk_action.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "kbx_buttons";

#define BTN_POLL_MS     20
#define BTN_LONG_US     800000
#define BTN_VOLUME_STEP 8
#define BTN_VOLUME_WRAP 20

static esp_err_t s_init_err = ESP_ERR_INVALID_STATE;
static volatile uint32_t s_key_taps, s_key_holds, s_boot_taps, s_boot_holds;

i2c_master_bus_handle_t touch_i2c_bus(void) { return NULL; }
bool touch_is_locked(void) { return false; }
void touch_set_locked(bool locked) { (void)locked; }
bool touch_point_on_orb(int x, int y) { (void)x; (void)y; return false; }

void touch_get_point(int *x, int *y, bool *pressed)
{
    if (x) *x = 0;
    if (y) *y = 0;
    if (pressed) *pressed = false;
}

void touch_log_status(void)
{
    ESP_LOGI(TAG, "buttons: init=%s key=%d boot=%d taps=%u holds=%u boot_taps=%u boot_holds=%u",
             esp_err_to_name(s_init_err), gpio_get_level(KEY_BUTTON_GPIO),
             gpio_get_level(BOOT_BUTTON_GPIO), (unsigned)s_key_taps,
             (unsigned)s_key_holds, (unsigned)s_boot_taps, (unsigned)s_boot_holds);
}

static void boot_volume_step(void)
{
    const uint32_t vol = audio_volume() >= 100 ? audio_set_volume(BTN_VOLUME_WRAP)
                                               : audio_adjust_volume(BTN_VOLUME_STEP);
    ui_notify_swipe(+1);
    ESP_LOGI(TAG, "boot -> volume=%u", (unsigned)vol);
}

/* Same path as "KBX cloud pair" (usb_log.c): a paired box shows its
 * account sheet, an unpaired one starts the device grant and shows the QR. */
static void boot_pairing(void)
{
    if (ui_pairing_is_open()) {
        ui_notify_pairing_close();
        ESP_LOGI(TAG, "boot hold -> pairing sheet closed");
        return;
    }
    cloud_auth_status_t status;
    cloud_auth_get_status(&status);
    if (!status.paired && !status.pairing) {
        if (net_ws_use_cloud() != ESP_OK || cloud_auth_start_pairing() != ESP_OK) {
            ESP_LOGW(TAG, "boot hold -> pairing could not start");
            ui_notify_talk_denied(I18N_ERR_OFFLINE);
            return;
        }
        device_config_reset();   /* another account's revisions start fresh */
    }
    ui_notify_cloud_pairing();
    ESP_LOGI(TAG, "boot hold -> pairing sheet (paired=%d)", (int)status.paired);
}

static void buttons_task(void *arg)
{
    (void)arg;
    int64_t key_down_us = 0, boot_down_us = 0;
    bool key_held = false, key_hold_fired = false, boot_held = false, boot_hold_fired = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
        const int64_t now = esp_timer_get_time();
        const bool key = gpio_get_level(KEY_BUTTON_GPIO) == 0;
        const bool boot = gpio_get_level(BOOT_BUTTON_GPIO) == 0;

        if (key && !key_held) {
            key_down_us = now;
            key_hold_fired = false;
        } else if (key && !key_hold_fired && now - key_down_us >= BTN_LONG_US) {
            key_hold_fired = true;
            s_key_holds++;
            talk_action_hold_mute();
        } else if (!key && key_held && !key_hold_fired) {
            s_key_taps++;
            if (ui_feed_popup_dismiss()) {
                ESP_LOGI(TAG, "key -> feed popup dismissed");
            } else {
                ui_notify_tap();
                talk_action_tap();
            }
        }
        key_held = key;

        if (boot && !boot_held) {
            boot_down_us = now;
            boot_hold_fired = false;
        } else if (boot && !boot_hold_fired && now - boot_down_us >= BTN_LONG_US) {
            boot_hold_fired = true;
            s_boot_holds++;
            boot_pairing();
        } else if (!boot && boot_held && !boot_hold_fired) {
            s_boot_taps++;
            boot_volume_step();
        }
        boot_held = boot;
    }
}

esp_err_t touch_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << KEY_BUTTON_GPIO) | (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    s_init_err = gpio_config(&cfg);
    if (s_init_err != ESP_OK) return s_init_err;
    if (xTaskCreate(buttons_task, "kbx_buttons", 4096, NULL, 5, NULL) != pdPASS) {
        s_init_err = ESP_ERR_NO_MEM;
        return s_init_err;
    }
    ESP_LOGI(TAG, "buttons up: KEY GPIO%d tap=talk hold=mute, BOOT GPIO%d tap=volume hold=pairing",
             KEY_BUTTON_GPIO, BOOT_BUTTON_GPIO);
    return ESP_OK;
}
