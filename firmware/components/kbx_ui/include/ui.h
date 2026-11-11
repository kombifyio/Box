#pragma once
#include <stdbool.h>
#include "i18n_ids.h"

/* UI 2 (UI design §2,
 * mockup frames R-1 ... R-11):
 *   ui_scene.c     the voice scene: edge ring, Aura Orb with the kombify
 *                  rosette, top chrome, caption, status pill, error line,
 *                  mute chrome and the transient volume/info/lock hints.
 *   ui_overlays.c  full-screen sheets: Quick Settings, pairing (R-8),
 *                  update (R-10); touch indev; transient notify state.
 * Layout comes from the display profile (display_profile.h). */

/* Forward decl so ui.h stays includable without LVGL headers (touch.c only
 * uses the notify_* hooks). Identical to LVGL's own typedef. */
typedef struct _lv_display_t lv_display_t;

/* Build the scene and then the overlays on the active screen. Called from
 * display_init while the LVGL port lock is held. */
void ui_scene_build(lv_display_t *disp);
void ui_scene_log_status(void);
/* Start the UI stall watchdog (esp_timer; logs ui_timer_stall). */
void ui_scene_health_start(void);

/* Build the Quick Settings, pairing and update sheets, register the LVGL
 * touch indev, and start a 30 ms timer for smooth panel motion. */
void ui_overlays_build(lv_display_t *disp);
/* LAN pairing sheet (ui_lan_pair.c): the homelab server's approval code. */
typedef struct _lv_obj_t lv_obj_t;
void ui_lan_pair_build(lv_obj_t *scr);
/* True while the LAN pairing sheet shows an approval code (LVGL context). */
bool ui_lan_pair_is_visible(void);

/* Transient hints the scene renders in its own zones (LVGL context). */
typedef struct {
    bool volume;            /* volume swipe: pill + ring show the volume  */
    bool info;              /* talk refused: caption shows info_id        */
    i18n_id_t info_id;
    bool lock;              /* child-lock tap: pill shows the unlock hint */
    int lock_remaining;     /* >0 taps left, 0 = unlocked                 */
} ui_transient_t;
void ui_overlays_transient(ui_transient_t *out);

/* Touch gesture hooks from touch.c (touch-task context). They only set flags
 * and timestamps — every LVGL mutation happens in the tick. */
void ui_notify_tap(void);        /* tap (talk start/stop runs in touch.c) */
void ui_notify_longpress(void);  /* same as talk_denied(I18N_NOT_READY) */
void ui_notify_talk_denied(i18n_id_t reason); /* one-line why talk did not start (localised at render) */
void ui_notify_swipe(int dir);   /* vertical swipe (+1 up / -1 down) */
void ui_notify_settings(bool open); /* set panel target state (animated) */
void ui_notify_cloud_pairing(void);
bool ui_pairing_is_open(void);
void ui_notify_pairing_close(void);
void ui_notify_updates(void);

/* Finger-following bottom sheet: touch.c reports progress 0..1000 during the
 * swipe stroke (0=closed, 1000=open); the 30 ms timer positions the panel
 * 1:1 with the finger. drag_end snaps open or closed with animation. */
void ui_notify_panel_drag(int progress);
void ui_notify_panel_drag_end(bool open);

/* Current panel position 0..1000 (live, not the target). touch.c anchors a
 * new drag to this so re-grabbing mid-animation does not jump. */
int ui_panel_progress(void);

/* Tap in child lock: briefly show an unlock hint (remaining>0 = "N left",
 * remaining==0 = "unlocked"). No persistent lock screen — the normal UI
 * stays visible; only touch is dead. */
void ui_notify_lock_tap(int remaining);

/* true while the Quick Settings panel (or any sheet: pairing, update, Feed
 * popup) is open. touch.c then pauses its global gestures (LVGL owns the
 * input, so a tap dismisses the Feed popup instead of starting a talk). */
bool ui_settings_is_open(void);
/* Feed popup (feed_popup.h): a key press dismisses it. Returns true when a
 * popup was open, so the press does nothing else. */
bool ui_feed_popup_dismiss(void);
void ui_overlays_log_status(void);
