/*
 * kombify box - minimal BLE radio (NimBLE) for the Bluetooth tile.
 *
 * Radio on/off only: the box advertises as "kombify box" (connectable
 * undirected), without a GATT service. Lazy: the stack starts on first
 * enable. All errors are non-fatal (no abort) — see bt_radio.h.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "bt_radio.h"

static const char *TAG = "kbx_bt";
static const char *DEVICE_NAME = "kombify box";

static volatile bool s_inited = false;   /* NimBLE stack started (once) */
static volatile bool s_on = false;       /* radio should advertise */
static volatile bool s_bringup_active = false;
static volatile bool s_released = false;  /* BT-controller RAM released -> BLE off */
static uint8_t s_addr_type;

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv = { 0 };
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, NULL, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as '%s'", DEVICE_NAME);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGW(TAG, "infer_addr rc=%d", rc);
        return;
    }
    if (s_on) {
        start_advertising();
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();               /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void bt_radio_init(void)
{
    /* lazy: only bt_radio_set_enabled(true) starts the stack */
}

/* Return the internal RAM reserved by the BT controller (~large block) to
 * the heap. Call early in boot, BEFORE the controller is ever inited —
 * after that the BLE tile stays off. Deliberate M1 trade: standalone voice
 * (net_ws + second TLS handshake) needs internal RAM more than the BLE
 * tile, which does not work anyway. */
void bt_radio_release(void)
{
    if (s_released || s_inited) {
        return;
    }
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    if (err == ESP_OK) {
        s_released = true;
        ESP_LOGI(TAG, "BT controller RAM released (BLE tile off, internal RAM for net_ws)");
    } else {
        ESP_LOGW(TAG, "bt mem release: %s", esp_err_to_name(err));
    }
}

static bool ensure_stack(void)
{
    if (s_inited) {
        return true;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    nimble_port_freertos_init(host_task);
    s_inited = true;
    return true;
}

/* One-shot stack bring-up in its OWN task: nimble_port_init + controller
 * enable need ~1-2 KB stack and block for tens of ms — both a poor fit in
 * the LVGL task (7 KB stack, holds the port lock). On first start on_sync
 * takes over advertising; this task only advertises if the stack happens
 * to already be synced. The task re-reads s_on at the end, so it catches
 * a disable that happened in between. */
static void bt_bringup_task(void *arg)
{
    (void)arg;
    ensure_stack();
    if (s_on && s_inited && ble_hs_synced()) {
        start_advertising();
    }
    s_bringup_active = false;
    vTaskDelete(NULL);
}

void bt_radio_set_enabled(bool on)
{
    if (s_released) {
        return; /* BT RAM released (M1: RAM for net_ws) — tile is a no-op */
    }
    if (on == s_on) {
        return;
    }
    s_on = on;
    if (!on) {
        if (s_inited) {
            ble_gap_adv_stop();
            ESP_LOGI(TAG, "BLE radio off");
        }
        return;
    }
    if (s_inited) {
        /* Re-enable: stack already running — advertise directly (lightweight). */
        if (ble_hs_synced()) {
            start_advertising();
        }
    } else if (!s_bringup_active) {
        /* First start: move heavy bring-up out of the LVGL task. */
        s_bringup_active = true;
        if (xTaskCreate(bt_bringup_task, "kbx_bt_up", 4096, NULL, 4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "bt bringup task create failed");
            s_bringup_active = false;
            s_on = false;
        }
    }
    /* If a bring-up is running (s_bringup_active) it applies the final
     * s_on itself — no second task, no double init. */
}

bool bt_radio_is_enabled(void)
{
    return s_on;
}
