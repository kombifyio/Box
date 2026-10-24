/*
 * kombify box - UI 2 full-screen sheets + transient notify state. See ui.h.
 *
 * Sheets (opaque, so LVGL skips the scene underneath):
 *   - Quick Settings : finger-following sheet (pull down from the top
 *                      edge/handle zone to open, pull up to close; snaps on
 *                      release). Handle, conversation button, volume slider,
 *                      2x2 tiles Wi-Fi / Cloud / mute / child lock.
 *   - Pairing (R-8)  : QR of verification_uri_complete, user code, URL, ring
 *                      counting down expires_in; connected / manage states.
 *   - Update (R-10)  : ring progress, large percentage, status, version.
 * Transient hints (volume, talk refused, child-lock taps) are only state
 * here; ui_scene.c renders them in the scene's own zones.
 *
 * Motion: overlay timer 80 ms idle, 30 ms during drag/animation. During a
 * drag the sheet follows the finger 1:1 (s_drag_progress from touch.c);
 * without drag s_progress eases toward the target.
 *
 * Threading: touch.c calls notify_* on the touch task and only sets volatile
 * flags/progress. EVERY LVGL mutation happens in the overlay timer (LVGL
 * context); button/backdrop events run in the indev context.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "audio.h"
#include "cloud_auth.h"
#include "display_profile.h"
#include "fw_ota.h"
#include "i18n.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "touch.h"
#include "ui.h"
#include "ui_style.h"
#include "ui_theme.h"
#include "device_config.h"
#include "wakeword.h"

/* --- timing (us) --- */
#define VOLHINT_US   1600000
#define INFO_US      1600000
#define LOCKHINT_US  1600000

/* --- sheet motion: the sheet spans the screen and slides in from above --- */
#define OPEN_YOFF    0
#define CLOSED_YOFF  (-(int)kbx_display_profile()->h - 10)

/* Ignore backdrop clicks shortly after a drag: the LVGL indev sees the
 * drag stroke (or a touch dropout in the middle) as press+release on the
 * backdrop and would immediately close the freshly opened panel
 * (user report 2026-08-21: "buttons briefly visible, then gone"). */
#define BACKDROP_GRACE_US 600000

/* --- Quick Settings geometry on the 360 design grid (kbx_dp) --- */
#define QS_HANDLE_Y   22
#define QS_TALK_Y     42
#define QS_WAKE_Y     90
#define QS_VOL_Y      144
#define QS_TILES_Y    166
#define QS_W          216
#define TILE_W        104
#define TILE_H        60
#define TILE_GAP      8
#define HANDLE_COL    0x3A4558

/* notify state (touch task writes, timer reads) */
static volatile int64_t s_volhint_until;
static volatile int64_t s_info_until;
static volatile int64_t s_lockhint_until;
static volatile int     s_lock_remaining;
static volatile bool    s_panel_open;        /* target state */
static volatile bool    s_drag_active;       /* finger is dragging */
static volatile int     s_drag_progress;     /* 0..1000 live from the finger */
static volatile i18n_id_t s_denied_id = I18N_NOT_READY;

/* Quick Settings */
static lv_obj_t *s_backdrop;
static lv_obj_t *s_panel;
static lv_obj_t *s_vol_slider;
static lv_obj_t *s_talk_btn;
static lv_obj_t *s_talk_lbl;
static lv_obj_t *s_tile_wifi;
static lv_obj_t *s_tile_bt;
static lv_obj_t *s_tile_mic;
/* Tile captions, in build order (wifi, cloud, mute, child lock). */
#define TILE_COUNT 4
static const i18n_id_t s_tile_ids[TILE_COUNT] = {
    I18N_TILE_WIFI, I18N_TILE_CLOUD, I18N_TILE_MUTE, I18N_TILE_CHILD_LOCK,
};
static lv_obj_t *s_tile_lbl[TILE_COUNT];
static lv_obj_t *s_tile_icon[TILE_COUNT];
static int s_talk_shown = -1;
/* Wake word row: label + switch; -1 = repaint */
static lv_obj_t *s_wake_row, *s_wake_lbl, *s_wake_track, *s_wake_knob;
static int s_wake_shown = -1;   /* 2=active, 1=ready, 0=not ready; -1 = repaint */
static volatile int64_t s_drag_end_us;       /* last panel drag release */
static volatile int s_progress;              /* current panel position 0..1000 */
static int       s_prev_progress = -1;

/* Pairing (R-8) */
static volatile bool s_pair_open;
static lv_obj_t *s_pair_card;
static lv_obj_t *s_pair_ring;
static lv_obj_t *s_pair_title;
static lv_obj_t *s_pair_qr;
static lv_obj_t *s_pair_code;
static lv_obj_t *s_pair_url;
static lv_obj_t *s_pair_hint;
static lv_obj_t *s_pair_badge;      /* tinted circle with a check (connected) */
static lv_obj_t *s_pair_forget;     /* main action: Connect / Done */
static lv_obj_t *s_pair_forget_label;
static lv_obj_t *s_pair_success;    /* kept name: the badge's check glyph */
static lv_obj_t *s_pair_disconnect;
static lv_obj_t *s_pair_disconnect_label;
static lv_obj_t *s_pair_close;
static lv_obj_t *s_pair_close_label;
static bool s_pair_manage;
static int64_t s_pair_success_until;
static lv_obj_t *s_pair_updates;
static lv_obj_t *s_pair_updates_label;
static char s_pair_url_drawn[256];
static int64_t s_pair_code_since;   /* when the current user code appeared */
static char s_pair_code_for[32];
static cloud_auth_status_t s_pair_status;
static bool s_pair_was_open;
static bool s_pair_drawn_paired;
static bool s_pair_drawn_pairing;
static bool s_pair_drawn_manage;
static char s_pair_drawn_code[32];
static char s_pair_drawn_error[64];
static uint32_t s_pair_locale_epoch = UINT32_MAX;
static volatile bool s_pair_action_busy;

/* Update (R-10) */
static volatile bool s_update_open;
static lv_obj_t *s_update_card;
static lv_obj_t *s_update_ring;
static lv_obj_t *s_update_icon;
static lv_obj_t *s_update_pct;
static lv_obj_t *s_update_title;
static lv_obj_t *s_update_status;    /* kept name: the version / keep-plugged sub line */
static lv_obj_t *s_update_version;
static lv_obj_t *s_update_action;
static lv_obj_t *s_update_action_label;
static lv_obj_t *s_update_close;
static lv_obj_t *s_update_close_label;
static fw_ota_update_status_t s_update_drawn;
static bool s_update_was_open;
static uint32_t s_update_locale_epoch = UINT32_MAX;

/* Flash commits and connection teardown must never run in an LVGL event.
 * Closing the modal remains immediate while this bounded action runs. */
static void pairing_action_task(void *arg)
{
    if ((uintptr_t)arg == 1) {
        /* The approving account may be another one: its revisions start fresh. */
        if (net_ws_use_cloud() == ESP_OK && cloud_auth_start_pairing() == ESP_OK) device_config_reset();
    } else {
        (void)net_ws_set_agent_enabled(false);
        (void)cloud_auth_forget();
        device_config_reset();
    }
    s_pair_action_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void pairing_action(bool start)
{
    if (s_pair_action_busy) return;
    s_pair_action_busy = true;
    if (xTaskCreatePinnedToCoreWithCaps(pairing_action_task, "pair_action", 4096,
            (void *)(uintptr_t)(start ? 1 : 2), 3, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_pair_action_busy = false;
    }
}

/* ------------------------------------------------------------ touch indev */

static void touch_indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0, y = 0;
    bool pressed = false;
    touch_get_point(&x, &y, &pressed);
    /* Orb taps belong to talk, not LVGL. */
    if (pressed && !ui_settings_is_open() && touch_point_on_orb(x, y)) {
        pressed = false;
    }
    data->point.x = (int32_t)x;
    data->point.y = (int32_t)y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ------------------------------------------------------- control callbacks */

static void on_vol_changed(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    audio_set_volume((uint32_t)lv_slider_get_value(s));
}

static void on_tile_wifi(lv_event_t *e) { (void)e; net_wifi_set_enabled(!net_wifi_is_enabled()); }
static void on_tile_mic(lv_event_t *e)  { (void)e; audio_set_local_mute(!audio_local_mute()); }
static void on_tile_lock(lv_event_t *e) { (void)e; touch_set_locked(true); s_panel_open = false; }
static void on_tile_bt(lv_event_t *e)
{
    (void)e;
    s_pair_open = true;
    s_panel_open = false;
    cloud_auth_get_status(&s_pair_status);
    s_pair_manage = s_pair_status.paired;
    s_pair_success_until = 0;
    if (!s_pair_status.paired && !s_pair_status.pairing) {
        pairing_action(true);
    }
}

static void on_pair_close(lv_event_t *e)
{
    (void)e;
    s_pair_open = false;
    s_panel_open = false;
}

static void on_pair_forget(lv_event_t *e)
{
    (void)e;
    cloud_auth_status_t status;
    cloud_auth_get_status(&status);
    if (status.paired) {
        s_pair_open = false;
        s_panel_open = false;
    } else if (!status.pairing) pairing_action(true);
}

static void on_pair_disconnect(lv_event_t *e)
{
    (void)e;
    s_pair_manage = false;
    pairing_action(false);
}

static void on_pair_updates(lv_event_t *e)
{
    (void)e;
    ui_notify_updates();
    (void)fw_ota_check_update();
}

static void on_update_close(lv_event_t *e)
{
    (void)e;
    fw_ota_update_status_t status;
    fw_ota_get_update_status(&status);
    if (status.state != FW_OTA_UPDATE_INSTALLING && status.state != FW_OTA_UPDATE_REBOOTING) {
        s_update_open = false;
    }
}

static void on_update_action(lv_event_t *e)
{
    (void)e;
    fw_ota_update_status_t status;
    fw_ota_get_update_status(&status);
    if (status.state == FW_OTA_UPDATE_AVAILABLE) (void)fw_ota_install_update();
    else if (status.state == FW_OTA_UPDATE_CURRENT) s_update_open = false;
    else if (status.state == FW_OTA_UPDATE_IDLE || status.state == FW_OTA_UPDATE_ERROR) {
        (void)fw_ota_check_update();
    }
}

/* Switch: wake word on/off. */
static void on_wake_clicked(lv_event_t *e)
{
    (void)e;
    wakeword_status_t st;
    wakeword_get_status(&st);
    if (wakeword_set_enabled(!st.enabled) == ESP_OK) device_config_note_local_change();
}

/* Row (name and phrase): next phrase; applies without a reboot. */
static void on_wake_phrase_clicked(lv_event_t *e)
{
    (void)e;
    int n = wakeword_phrase_count();
    if (n > 1 && wakeword_set_phrase((wakeword_get_phrase() + 1) % n) == ESP_OK) {
        device_config_note_local_change();
    }
}

static void on_talk_clicked(lv_event_t *e)
{
    (void)e;
    if (net_ws_conversation_active()) {
        net_ws_talk_cancel();
    } else if (net_ws_conversation_start()) {
        s_panel_open = false;
    }
}

static void on_backdrop_clicked(lv_event_t *e)
{
    (void)e;
    /* Do not close during/just after a drag or before the panel is open
     * at all — otherwise the drag release closes it immediately. */
    if (s_drag_active || s_progress < 1000 ||
        esp_timer_get_time() - s_drag_end_us < BACKDROP_GRACE_US) {
        return;
    }
    s_panel_open = false;
}

/* --------------------------------------------------------------- builders */

/* Tile: icon over caption, kombify finish (opaque, hairline, radius 16). */
static lv_obj_t *make_tile(lv_obj_t *parent, const char *icon, int idx, lv_event_cb_t cb)
{
    lv_obj_t *t = lv_button_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(t, kbx_dp(TILE_W), kbx_dp(TILE_H));
    lv_obj_set_style_radius(t, 16, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_opa(t, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ic = lv_label_create(t);
    lv_obj_remove_style_all(ic);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 8);
    s_tile_icon[idx] = ic;

    /* Caption width = tile inner width: longer translations end in "..." */
    lv_obj_t *lb = kbx_label(t, I18N_FONT_14, KBX_COL_TEXT_MID);
    lv_label_set_text(lb, i18n_str(s_tile_ids[idx]));
    lv_obj_set_width(lb, kbx_dp(TILE_W) - 12);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, -7);
    s_tile_lbl[idx] = lb;
    return t;
}

typedef enum { TILE_OFF, TILE_ON, TILE_ALERT } tile_look_t;

static void tile_set_state(lv_obj_t *t, int idx, tile_look_t look)
{
    uint32_t bg = KBX_COL_SURF, line = KBX_COL_LINE, txt = KBX_COL_TEXT_MID, ic = 0x9AA3B5;
    if (look == TILE_ON) {
        bg = KBX_COL_TINT; line = KBX_COL_TINT_LINE; txt = KBX_COL_TEXT_HI; ic = KBX_COL_BRAND;
    } else if (look == TILE_ALERT) {
        bg = KBX_COL_ALERT_BG; line = KBX_COL_ALERT_LINE; txt = KBX_COL_ALERT; ic = KBX_COL_ALERT;
    }
    lv_obj_set_style_bg_color(t, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(t, lv_color_hex(line), 0);
    lv_obj_set_style_text_color(s_tile_lbl[idx], lv_color_hex(txt), 0);
    lv_obj_set_style_text_color(s_tile_icon[idx], lv_color_hex(ic), 0);
}

static void build_panel(lv_obj_t *scr)
{
    s_backdrop = kbx_sheet(scr);
    lv_obj_add_event_cb(s_backdrop, on_backdrop_clicked, LV_EVENT_CLICKED, NULL);

    s_panel = kbx_box(scr);
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, CLOSED_YOFF);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *handle = kbx_box(s_panel);
    lv_obj_set_size(handle, 36, 4);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(HANDLE_COL), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, kbx_dp(QS_HANDLE_Y));

    /* Optional conversation entry; the orb keeps its default OneShot tap. */
    s_talk_btn = kbx_button(s_panel, KBX_BTN_TINT, kbx_dp(QS_W - 8), on_talk_clicked, &s_talk_lbl);
    lv_label_set_text(s_talk_lbl, i18n_str(I18N_CONVERSATION));
    lv_obj_align(s_talk_btn, LV_ALIGN_TOP_MID, 0, kbx_dp(QS_TALK_Y));

    /* Wake word on/off: "Wake word · Hi ESP" with a switch (owner setting). */
    s_wake_row = lv_button_create(s_panel);
    lv_obj_remove_style_all(s_wake_row);
    lv_obj_remove_flag(s_wake_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(s_wake_row, kbx_dp(QS_W - 8), 40);
    lv_obj_set_style_radius(s_wake_row, 14, 0);
    lv_obj_set_style_bg_color(s_wake_row, lv_color_hex(KBX_COL_SURF), 0);
    lv_obj_set_style_bg_opa(s_wake_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_wake_row, lv_color_hex(KBX_COL_LINE), 0);
    lv_obj_set_style_border_width(s_wake_row, 1, 0);
    lv_obj_set_style_opa(s_wake_row, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_wake_row, on_wake_phrase_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_wake_row, LV_ALIGN_TOP_MID, 0, kbx_dp(QS_WAKE_Y));
    s_wake_lbl = kbx_label(s_wake_row, I18N_FONT_14, KBX_COL_TEXT_MID);
    lv_obj_set_width(s_wake_lbl, kbx_dp(QS_W - 8) - 70);
    lv_label_set_long_mode(s_wake_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_wake_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_wake_lbl, LV_ALIGN_LEFT_MID, 14, 0);
    s_wake_track = kbx_box(s_wake_row);
    lv_obj_set_size(s_wake_track, 38, 22);
    lv_obj_set_style_radius(s_wake_track, 11, 0);
    lv_obj_set_style_bg_opa(s_wake_track, LV_OPA_COVER, 0);
    lv_obj_align(s_wake_track, LV_ALIGN_RIGHT_MID, -12, 0);
    /* The switch is its own touch target (the row cycles the phrase). */
    lv_obj_add_flag(s_wake_track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_wake_track, 10);
    lv_obj_add_event_cb(s_wake_track, on_wake_clicked, LV_EVENT_CLICKED, NULL);
    s_wake_knob = kbx_box(s_wake_track);
    lv_obj_set_size(s_wake_knob, 16, 16);
    lv_obj_set_style_radius(s_wake_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_wake_knob, LV_OPA_COVER, 0);

    lv_obj_t *vrow = kbx_box(s_panel);
    lv_obj_set_size(vrow, kbx_dp(QS_W), 24);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vrow, 14, 0);
    lv_obj_set_style_pad_hor(vrow, 4, 0);
    lv_obj_align(vrow, LV_ALIGN_TOP_MID, 0, kbx_dp(QS_VOL_Y) - 12);
    lv_obj_t *vsym = kbx_label(vrow, I18N_FONT_16, KBX_COL_TEXT_LO);
    lv_obj_set_style_text_font(vsym, &lv_font_montserrat_16, 0);
    lv_label_set_text(vsym, LV_SYMBOL_VOLUME_MAX);

    /* Thin slider: 6 px track, brand fill, 20 px knob, no theme. */
    s_vol_slider = lv_slider_create(vrow);
    lv_obj_remove_style_all(s_vol_slider);
    lv_obj_set_height(s_vol_slider, 6);
    lv_obj_set_flex_grow(s_vol_slider, 1);
    lv_obj_set_ext_click_area(s_vol_slider, 14);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(KBX_COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_radius(s_vol_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(KBX_COL_BRAND), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_vol_slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(KBX_COL_TEXT_HI), LV_PART_KNOB);
    lv_obj_set_style_radius(s_vol_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_slider, 7, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, on_vol_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *grid = kbx_box(s_panel);
    lv_obj_set_size(grid, kbx_dp(TILE_W) * 2 + TILE_GAP, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, TILE_GAP, 0);
    lv_obj_set_style_pad_row(grid, TILE_GAP, 0);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, kbx_dp(QS_TILES_Y));

    s_tile_wifi = make_tile(grid, LV_SYMBOL_WIFI, 0, on_tile_wifi);
    s_tile_bt = make_tile(grid, LV_SYMBOL_UPLOAD, 1, on_tile_bt);
    s_tile_mic = make_tile(grid, LV_SYMBOL_MUTE, 2, on_tile_mic);
    lv_obj_t *lock = make_tile(grid, LV_SYMBOL_EYE_CLOSE, 3, on_tile_lock);
    tile_set_state(s_tile_wifi, 0, TILE_OFF);
    tile_set_state(s_tile_bt, 1, TILE_OFF);
    tile_set_state(s_tile_mic, 2, TILE_OFF);
    tile_set_state(lock, 3, TILE_OFF);
}

/* Full-screen sheet with its own edge ring (R-8/R-10 progress meters). */
static lv_obj_t *make_ring_sheet(lv_obj_t *scr, lv_obj_t **ring)
{
    const kbx_display_profile_t *p = kbx_display_profile();
    lv_obj_t *s = kbx_sheet(scr);
    *ring = NULL;
    if (p->ring_d) {
        lv_obj_t *r = kbx_ring(s, p->ring_d, p->ring_w);
        lv_arc_set_rotation(r, 270);
        lv_arc_set_bg_angles(r, 0, 360);
        lv_obj_set_style_arc_color(r, lv_color_hex(KBX_COL_BRAND), LV_PART_INDICATOR);
        *ring = r;
    }
    return s;
}

static void ring_progress(lv_obj_t *ring, int permille)
{
    if (!ring) return;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    if (lv_arc_get_value(ring) == permille) return;
    lv_obj_set_style_arc_opa(ring, permille > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_arc_set_value(ring, permille);
}

static void build_pairing(lv_obj_t *scr)
{
    s_pair_card = make_ring_sheet(scr, &s_pair_ring);

    s_pair_title = kbx_text_block(s_pair_card, I18N_FONT_18, KBX_COL_TEXT_HI, kbx_dp(220));
    lv_obj_align(s_pair_title, LV_ALIGN_TOP_MID, 0, kbx_dp(38));

    /* QR: 140 px including a white quiet zone, radius 12 (mockup R-8). */
    s_pair_qr = lv_qrcode_create(s_pair_card);
    lv_qrcode_set_size(s_pair_qr, kbx_dp(124));
#if CONFIG_KBX_DISPLAY_MONO
    /* The RLCD flush inverts luminance (dark ground = paper). Swap the QR
     * colours so the panel shows dark modules on a light quiet zone, the
     * polarity every phone camera reads. */
    const lv_color_t qr_dark = lv_color_white(), qr_light = lv_color_hex(KBX_COL_BG);
#else
    const lv_color_t qr_dark = lv_color_hex(KBX_COL_BG), qr_light = lv_color_white();
#endif
    lv_qrcode_set_dark_color(s_pair_qr, qr_dark);
    lv_qrcode_set_light_color(s_pair_qr, qr_light);
    lv_obj_set_style_pad_all(s_pair_qr, 8, 0);
    lv_obj_set_style_radius(s_pair_qr, 12, 0);
    lv_obj_set_style_bg_color(s_pair_qr, qr_light, 0);
    lv_obj_set_style_bg_opa(s_pair_qr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pair_qr, 0, 0);
    lv_obj_align(s_pair_qr, LV_ALIGN_TOP_MID, 0, kbx_dp(70));

    s_pair_code = kbx_label(s_pair_card, I18N_FONT_16, KBX_COL_TEXT_HI);
    lv_obj_set_style_text_font(s_pair_code, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(s_pair_code, 2, 0);
    lv_obj_align(s_pair_code, LV_ALIGN_TOP_MID, 0, kbx_dp(220));

    s_pair_url = kbx_label(s_pair_card, I18N_FONT_16, KBX_COL_TEXT_LO);
    lv_obj_set_style_text_font(s_pair_url, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_pair_url, kbx_dp(250));
    lv_label_set_long_mode(s_pair_url, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_pair_url, LV_ALIGN_TOP_MID, 0, kbx_dp(260));

    /* connected badge: tinted circle with a brand check (R-9 language) */
    s_pair_badge = kbx_box(s_pair_card);
    lv_obj_set_size(s_pair_badge, kbx_dp(72), kbx_dp(72));
    lv_obj_set_style_radius(s_pair_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pair_badge, lv_color_hex(KBX_COL_TINT), 0);
    lv_obj_set_style_bg_opa(s_pair_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_pair_badge, lv_color_hex(KBX_COL_TINT_LINE), 0);
    lv_obj_set_style_border_width(s_pair_badge, 1, 0);
    s_pair_success = lv_label_create(s_pair_badge);
    lv_obj_remove_style_all(s_pair_success);
    lv_obj_set_style_text_font(s_pair_success, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_pair_success, lv_color_hex(KBX_COL_BRAND), 0);
    lv_label_set_text(s_pair_success, LV_SYMBOL_OK);
    lv_obj_center(s_pair_success);

    s_pair_hint = kbx_text_block(s_pair_card, I18N_FONT_16, KBX_COL_TEXT_MID, kbx_dp(236));

    s_pair_updates = kbx_button(s_pair_card, KBX_BTN_TINT, kbx_dp(200), on_pair_updates,
                                &s_pair_updates_label);
    s_pair_forget = kbx_button(s_pair_card, KBX_BTN_PRIMARY, kbx_dp(200), on_pair_forget,
                               &s_pair_forget_label);
    s_pair_disconnect = kbx_button(s_pair_card, KBX_BTN_TEXT, kbx_dp(200), on_pair_disconnect,
                                   &s_pair_disconnect_label);
    i18n_style_label(s_pair_disconnect_label, I18N_FONT_14);
    lv_obj_set_style_text_color(s_pair_disconnect_label, lv_color_hex(KBX_COL_TEXT_LO), 0);
    s_pair_close = kbx_button(s_pair_card, KBX_BTN_TEXT, kbx_dp(160), on_pair_close,
                              &s_pair_close_label);
}

static void build_updates(lv_obj_t *scr)
{
    s_update_card = make_ring_sheet(scr, &s_update_ring);

    s_update_icon = lv_label_create(s_update_card);
    lv_obj_remove_style_all(s_update_icon);
    lv_obj_set_style_text_font(s_update_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_update_icon, lv_color_hex(KBX_COL_BRAND), 0);
    lv_label_set_text(s_update_icon, LV_SYMBOL_DOWNLOAD);
    lv_obj_align(s_update_icon, LV_ALIGN_TOP_MID, 0, kbx_dp(70));

    extern const lv_font_t kbx_num_56;
    s_update_pct = kbx_label(s_update_card, I18N_FONT_16, KBX_COL_TEXT_HI);
    lv_obj_set_style_text_font(s_update_pct, &kbx_num_56, 0);
    lv_obj_align(s_update_pct, LV_ALIGN_TOP_MID, 0, kbx_dp(116));

    s_update_title = kbx_text_block(s_update_card, I18N_FONT_18, KBX_COL_TEXT_HI, kbx_dp(240));
    s_update_version = kbx_label(s_update_card, I18N_FONT_16, KBX_COL_TEXT_LO);
    lv_obj_set_style_text_font(s_update_version, &lv_font_montserrat_16, 0);
    s_update_status = kbx_text_block(s_update_card, I18N_FONT_16, KBX_COL_TEXT_LO, kbx_dp(230));

    s_update_action = kbx_button(s_update_card, KBX_BTN_PRIMARY, kbx_dp(200), on_update_action,
                                 &s_update_action_label);
    s_update_close = kbx_button(s_update_card, KBX_BTN_TEXT, kbx_dp(160), on_update_close,
                                &s_update_close_label);
}

/* Sheet position from progress (0=closed, 1000=open). The backdrop stays
 * deliberately FULLY OPAQUE (not gradual) once the sheet is visible: a
 * semi-transparent full-screen layer would force LVGL to recompose the orb
 * every frame -> stutter. Opaque uses LVGL's cover check so only the sheet
 * region is redrawn. */
static void panel_apply(int progress)
{
    int yoff = CLOSED_YOFF + progress * (OPEN_YOFF - CLOSED_YOFF) / 1000;
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, yoff);
    kbx_set_hidden(s_backdrop, progress <= 0);
    kbx_set_hidden(s_panel, progress <= 0);
}

int ui_panel_progress(void)
{
    return s_progress;
}

/* "https://login.kombify.io/activate" -> "login.kombify.io/activate" */
static const char *short_url(const char *url)
{
    const char *p = strstr(url, "://");
    return p ? p + 3 : url;
}

static void place(lv_obj_t *o, int y)
{
    lv_obj_align(o, LV_ALIGN_TOP_MID, 0, kbx_dp(y));
}

static void update_tick(void)
{
    kbx_set_hidden(s_update_card, !s_update_open);
    if (!s_update_open) {
        s_update_was_open = false;
        return;
    }
    fw_ota_update_status_t update;
    fw_ota_get_update_status(&update);
    bool changed = !s_update_was_open || s_update_locale_epoch != i18n_epoch() ||
        update.state != s_update_drawn.state ||
        update.progress_percent != s_update_drawn.progress_percent ||
        strcmp(update.version, s_update_drawn.version) != 0;
    s_update_was_open = true;
    if (!changed) return;

    lv_obj_move_foreground(s_update_card);
    i18n_style_label(s_update_title, I18N_FONT_18);
    i18n_style_label(s_update_status, I18N_FONT_16);
    i18n_style_label(s_update_action_label, I18N_FONT_16);
    i18n_style_label(s_update_close_label, I18N_FONT_16);
    bool installing = update.state == FW_OTA_UPDATE_INSTALLING || update.state == FW_OTA_UPDATE_REBOOTING;
    bool working = installing || update.state == FW_OTA_UPDATE_CHECKING;
    i18n_id_t message = update.state == FW_OTA_UPDATE_CURRENT ? I18N_UPDATE_CURRENT :
        update.state == FW_OTA_UPDATE_AVAILABLE ? I18N_UPDATE_AVAILABLE :
        update.state == FW_OTA_UPDATE_ERROR ? I18N_UPDATE_ERROR :
        installing ? I18N_UPDATE_INSTALLING : I18N_UPDATE_CHECKING;

    /* R-10 while installing: ring + large percentage + "keep plugged in",
     * no buttons (no cancel after the slot write begins). Otherwise the
     * icon, status, version and the next action. */
    kbx_set_hidden(s_update_pct, !installing);
    ring_progress(s_update_ring, installing ? (int)update.progress_percent * 10 : 0);
    if (installing) {
        char pct[12];
        snprintf(pct, sizeof(pct), "%u %%", (unsigned)update.progress_percent);
        lv_label_set_text(s_update_pct, pct);
        place(s_update_title, 196);
        place(s_update_version, 226);
        place(s_update_status, 254);
        lv_label_set_text(s_update_status, i18n_str(I18N_UI2_KEEP_PLUGGED));
        kbx_set_hidden(s_update_status, false);
    } else {
        place(s_update_title, 120);
        place(s_update_version, 176);
        kbx_set_hidden(s_update_status, true);
    }
    lv_label_set_text(s_update_title, i18n_str(message));
    lv_obj_set_style_base_dir(s_update_pct, LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_base_dir(s_update_version, LV_BASE_DIR_LTR, 0);
    lv_label_set_text(s_update_version, update.version);
    kbx_set_hidden(s_update_version, update.version[0] == '\0');

    kbx_set_hidden(s_update_action, installing);
    kbx_set_hidden(s_update_close, installing);
    place(s_update_action, 220);
    place(s_update_close, 270);
    lv_label_set_text(s_update_action_label, i18n_str(
        update.state == FW_OTA_UPDATE_AVAILABLE ? I18N_UPDATE_INSTALL :
        update.state == FW_OTA_UPDATE_CURRENT ? I18N_CLOUD_DONE : I18N_UPDATE_CHECK));
    kbx_button_set_kind(s_update_action, s_update_action_label,
        update.state == FW_OTA_UPDATE_AVAILABLE ? KBX_BTN_PRIMARY : KBX_BTN_TINT);
    lv_label_set_text(s_update_close_label, i18n_str(I18N_UI2_CLOSE));
    if (working) lv_obj_add_state(s_update_action, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_update_action, LV_STATE_DISABLED);
    if (installing) lv_obj_add_state(s_update_close, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_update_close, LV_STATE_DISABLED);
    s_update_drawn = update;
    s_update_locale_epoch = i18n_epoch();
}

static void pairing_tick(int64_t now)
{
    if (s_pair_open && s_pair_success_until && now >= s_pair_success_until) {
        s_pair_open = false;
        s_panel_open = false;
        s_pair_success_until = 0;
    }
    kbx_set_hidden(s_pair_card, !s_pair_open);

    bool have_code = s_pair_status.pairing && s_pair_status.user_code[0];
    /* countdown of expires_in from when this code first appeared */
    if (have_code && strcmp(s_pair_code_for, s_pair_status.user_code)) {
        strlcpy(s_pair_code_for, s_pair_status.user_code, sizeof(s_pair_code_for));
        s_pair_code_since = now;
    }
    if (s_pair_open && have_code && s_pair_status.expires_in > 0) {
        int64_t left = (int64_t)s_pair_status.expires_in * 1000000 - (now - s_pair_code_since);
        ring_progress(s_pair_ring, (int)(left * 1000 / ((int64_t)s_pair_status.expires_in * 1000000)));
    } else {
        ring_progress(s_pair_ring, 0);
    }

    bool pair_changed = !s_pair_was_open || s_pair_drawn_paired != s_pair_status.paired ||
        s_pair_drawn_pairing != s_pair_status.pairing ||
        s_pair_drawn_manage != s_pair_manage ||
        strcmp(s_pair_drawn_code, s_pair_status.user_code) != 0 ||
        strcmp(s_pair_drawn_error, s_pair_status.error_code) != 0 ||
        s_pair_locale_epoch != i18n_epoch();
    if (s_pair_open && pair_changed) {
        /* Reapplying styles/text every 80 ms forced a full QSPI redraw even
         * when a phone approval was merely pending. */
        if (!s_pair_was_open) lv_obj_move_foreground(s_pair_card);
        i18n_style_label(s_pair_title, I18N_FONT_18);
        i18n_style_label(s_pair_hint, I18N_FONT_16);
        i18n_style_label(s_pair_forget_label, I18N_FONT_16);
        i18n_style_label(s_pair_disconnect_label, I18N_FONT_14);
        i18n_style_label(s_pair_updates_label, I18N_FONT_16);
        i18n_style_label(s_pair_close_label, I18N_FONT_16);
        bool paired = s_pair_status.paired;
        bool manage = paired && s_pair_manage;

        lv_label_set_text(s_pair_title, i18n_str(paired ? I18N_CLOUD_CONNECTED : I18N_CLOUD_PAIR));
        lv_label_set_text(s_pair_forget_label, i18n_str(paired ? I18N_CLOUD_DONE : I18N_CLOUD_PAIR));
        lv_label_set_text(s_pair_disconnect_label, i18n_str(I18N_CLOUD_FORGET));
        lv_label_set_text(s_pair_updates_label, i18n_str(I18N_UPDATE_TITLE));
        lv_label_set_text(s_pair_close_label, i18n_str(I18N_UI2_CANCEL));
        lv_obj_set_style_base_dir(s_pair_code, LV_BASE_DIR_LTR, 0);
        lv_obj_set_style_base_dir(s_pair_url, LV_BASE_DIR_LTR, 0);
        i18n_id_t hint = paired ? I18N_CLOUD_READY :
            have_code ? I18N_CLOUD_SCAN : s_pair_status.pairing ? I18N_STATUS_CONNECTING : I18N_CLOUD_RETRY;
        lv_label_set_text(s_pair_hint, i18n_str(hint));

        kbx_set_hidden(s_pair_qr, !have_code);
        kbx_set_hidden(s_pair_code, !have_code);
        kbx_set_hidden(s_pair_url, !have_code);
        kbx_set_hidden(s_pair_badge, !paired);
        kbx_set_hidden(s_pair_hint, have_code);
        kbx_set_hidden(s_pair_forget, s_pair_status.pairing);
        kbx_set_hidden(s_pair_updates, !manage);
        kbx_set_hidden(s_pair_disconnect, !manage);
        kbx_set_hidden(s_pair_close, paired);

        if (have_code) {
            /* R-8: title, QR, code, URL, Cancel */
            place(s_pair_title, 38);
            place(s_pair_close, 290);
            lv_label_set_text(s_pair_code, s_pair_status.user_code);
            lv_label_set_text(s_pair_url, short_url(s_pair_status.verification_uri[0] ?
                s_pair_status.verification_uri : s_pair_status.verification_uri_complete));
            const char *url = s_pair_status.verification_uri_complete;
            if (strcmp(s_pair_url_drawn, url) != 0) {
                strlcpy(s_pair_url_drawn, url, sizeof(s_pair_url_drawn));
                lv_qrcode_update(s_pair_qr, url, strlen(url));
            }
        } else if (paired) {
            /* badge, "Connected", hint, then Updates / Done / Disconnect */
            place(s_pair_badge, manage ? 46 : 70);
            place(s_pair_title, manage ? 128 : 154);
            place(s_pair_hint, manage ? 158 : 186);
            place(s_pair_updates, 196);
            place(s_pair_forget, manage ? 244 : 236);
            place(s_pair_disconnect, 290);
            kbx_button_set_kind(s_pair_forget, s_pair_forget_label, KBX_BTN_PRIMARY);
        } else {
            /* waiting for a code, or failed: title, hint, retry, cancel */
            place(s_pair_title, 104);
            place(s_pair_hint, 140);
            place(s_pair_forget, 206);
            place(s_pair_close, s_pair_status.pairing ? 220 : 256);
            kbx_button_set_kind(s_pair_forget, s_pair_forget_label, KBX_BTN_PRIMARY);
        }
        if (!have_code) s_pair_url_drawn[0] = '\0';
        if (paired && !s_pair_drawn_paired && !s_pair_manage) {
            s_pair_success_until = now + 3000000;
        }
        s_pair_drawn_paired = s_pair_status.paired;
        s_pair_drawn_pairing = s_pair_status.pairing;
        s_pair_drawn_manage = s_pair_manage;
        strlcpy(s_pair_drawn_code, s_pair_status.user_code, sizeof(s_pair_drawn_code));
        strlcpy(s_pair_drawn_error, s_pair_status.error_code, sizeof(s_pair_drawn_error));
        s_pair_locale_epoch = i18n_epoch();
    }
    s_pair_was_open = s_pair_open;
}

/* Panel drag 30 ms; idle 80 ms. Constant flag toggling invalidates LVGL. */
static void overlays_timer_cb(lv_timer_t *t)
{
    int64_t now = esp_timer_get_time();
    bool busy = s_drag_active || (s_progress != (s_panel_open ? 1000 : 0));
    lv_timer_set_period(t, busy ? 30 : 80);
    cloud_auth_get_status(&s_pair_status);
    update_tick();
    pairing_tick(now);

    /* Locale switched (KBX locale / session-create): restyle the labels
     * that are not rewritten each tick and force the talk button repaint. */
    static uint32_t s_locale_epoch = UINT32_MAX;
    if (s_locale_epoch != i18n_epoch()) {
        s_locale_epoch = i18n_epoch();
        i18n_style_label(s_talk_lbl, I18N_FONT_16);
        for (int i = 0; i < TILE_COUNT; i++) {
            i18n_style_label(s_tile_lbl[i], I18N_FONT_14);
            lv_label_set_text(s_tile_lbl[i], i18n_str(s_tile_ids[i]));
        }
        s_talk_shown = -1;
        s_wake_shown = -1;
    }

    /* Panel motion: finger-follow or soft easing toward the target. */
    if (s_drag_active) {
        s_progress = s_drag_progress;
    } else {
        int target = s_panel_open ? 1000 : 0;
        if (s_progress != target) {
            int step = (target - s_progress) / 4;
            if (step == 0) step = (target > s_progress) ? 1 : -1;
            s_progress += step;
            if ((step > 0 && s_progress > target) || (step < 0 && s_progress < target)) {
                s_progress = target;
            }
        }
    }
    if (s_progress != s_prev_progress) {
        if (s_prev_progress <= 0 && s_progress > 0) {
            /* on open, set the slider to the current volume */
            lv_slider_set_value(s_vol_slider, (int32_t)audio_volume(), LV_ANIM_OFF);
        }
        panel_apply(s_progress);
        s_prev_progress = s_progress;
    }
    if (s_progress > 0) {
        /* Retile styles only on state change (not every frame). */
        static int s_wifi_shown = -1, s_bt_shown = -1, s_mic_shown = -1;
        int w = net_wifi_is_enabled() ? 1 : 0;
        int b = s_pair_status.paired ? 1 : 0;
        int m = audio_local_mute() ? 1 : 0;
        if (w != s_wifi_shown) {
            tile_set_state(s_tile_wifi, 0, w ? TILE_ON : TILE_OFF);
            s_wifi_shown = w;
        }
        if (b != s_bt_shown) {
            tile_set_state(s_tile_bt, 1, b ? TILE_ON : TILE_OFF);
            s_bt_shown = b;
        }
        if (m != s_mic_shown) {
            tile_set_state(s_tile_mic, 2, m ? TILE_ALERT : TILE_OFF);
            s_mic_shown = m;
        }
        /* Wake word switch: on = brand track, knob right; unavailable = dim. */
        wakeword_status_t ws;
        wakeword_get_status(&ws);
        int wake = (ws.enabled ? 1 : 0) | (ws.error != ESP_OK ? 2 : 0) |
            (wakeword_get_phrase() << 2);
        if (wake != s_wake_shown) {
            char text[96];
            /* Name over phrase: never wrap inside "Hi ESP". */
            /* The ">" marks the phrase as tappable (next phrase). */
            snprintf(text, sizeof(text), "%s\n\"%s\"%s", i18n_str(I18N_UI2_WAKEWORD),
                     wakeword_phrase_text(wakeword_get_phrase()),
                     wakeword_phrase_count() > 1 ? "  >" : "");
            i18n_style_label(s_wake_lbl, I18N_FONT_14);
            lv_obj_set_style_text_align(s_wake_lbl, LV_TEXT_ALIGN_LEFT, 0);
            lv_label_set_text(s_wake_lbl, text);
            bool on = wake & 1;
            lv_obj_set_style_bg_color(s_wake_track, lv_color_hex(on ? KBX_COL_BRAND : KBX_COL_LINE), 0);
            lv_obj_set_style_bg_color(s_wake_knob, lv_color_hex(on ? KBX_COL_BG : KBX_COL_TEXT_MID), 0);
            lv_obj_align(s_wake_knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -3 : 3, 0);
            lv_obj_set_style_text_color(s_wake_lbl, lv_color_hex(on ? KBX_COL_TEXT_HI : KBX_COL_TEXT_MID), 0);
            lv_obj_set_style_opa(s_wake_row, (wake & 2) && !on ? LV_OPA_50 : LV_OPA_COVER, 0);
            s_wake_shown = wake;
        }
        /* Stop stays available through listening, thinking and playback. */
        int talk = net_ws_conversation_active() ? 2 :
            ((net_ws_talk_active() || net_ws_ready_for_talk() || net_ws_is_connected()) ? 1 : 0);
        if (talk != s_talk_shown) {
            kbx_button_set_kind(s_talk_btn, s_talk_lbl, talk == 2 ? KBX_BTN_PRIMARY : KBX_BTN_TINT);
            if (talk == 0) lv_obj_add_state(s_talk_btn, LV_STATE_DISABLED);
            else lv_obj_remove_state(s_talk_btn, LV_STATE_DISABLED);
            lv_label_set_text(s_talk_lbl,
                i18n_str(talk == 2 ? I18N_CONVERSATION_STOP : I18N_CONVERSATION));
            s_talk_shown = talk;
        }
    }
}

void ui_overlays_build(lv_display_t *disp)
{
    lv_obj_t *scr = lv_screen_active();
    build_panel(scr);
    build_pairing(scr);
    build_updates(scr);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_indev_read);
    lv_indev_set_display(indev, disp);

    lv_timer_create(overlays_timer_cb, 80, NULL);
}

void ui_overlays_transient(ui_transient_t *out)
{
    int64_t now = esp_timer_get_time();
    bool closed = s_progress == 0 && !s_pair_open && !s_update_open;
    memset(out, 0, sizeof(*out));
    out->lock = now < s_lockhint_until;
    out->lock_remaining = s_lock_remaining;
    out->volume = closed && now < s_volhint_until;
    out->info = closed && !out->volume && now < s_info_until;
    out->info_id = s_denied_id;
}

/* ----------------------------------------------------- notify hooks (touch) */

void ui_notify_tap(void) {}

void ui_notify_talk_denied(i18n_id_t reason)
{
    s_denied_id = reason;
    s_info_until = esp_timer_get_time() + INFO_US;
}

void ui_notify_longpress(void)
{
    ui_notify_talk_denied(I18N_NOT_READY);
}

void ui_notify_swipe(int dir)
{
    (void)dir;   /* touch.c already changed volume; just display it */
    s_volhint_until = esp_timer_get_time() + VOLHINT_US;
}

void ui_notify_settings(bool open)
{
    s_panel_open = open;   /* timer eases toward the target */
    if (!open) {
        s_pair_open = false;
        fw_ota_update_status_t status;
        fw_ota_get_update_status(&status);
        if (status.state != FW_OTA_UPDATE_INSTALLING && status.state != FW_OTA_UPDATE_REBOOTING) {
            s_update_open = false;
        }
    }
}

void ui_notify_panel_drag(int progress)
{
    s_drag_progress = progress;
    s_drag_active = true;
}

void ui_notify_panel_drag_end(bool open)
{
    s_drag_active = false;
    s_drag_end_us = esp_timer_get_time();  /* backdrop-click grace */
    s_panel_open = open;   /* timer snaps from the current position */
}

void ui_notify_lock_tap(int remaining)
{
    s_lock_remaining = remaining;
    s_lockhint_until = esp_timer_get_time() + LOCKHINT_US;
}

bool ui_settings_is_open(void)
{
    return s_panel_open || s_pair_open || s_update_open;
}

void ui_overlays_log_status(void)
{
    ESP_LOGI("kbx_display", "overlays: settings=%d pairing=%d updates=%d panel=%d",
             s_panel_open, s_pair_open, s_update_open, s_progress);
}

void ui_notify_updates(void)
{
    s_pair_open = false;
    s_panel_open = false;
    s_update_open = true;
}

bool ui_pairing_is_open(void)
{
    return s_pair_open;
}

void ui_notify_pairing_close(void)
{
    s_pair_open = false;
    s_panel_open = false;
}

void ui_notify_cloud_pairing(void)
{
    s_pair_manage = false;
    s_pair_success_until = 0;
    s_pair_open = true;
    s_panel_open = false;
}
