#include "c6_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include "assets/kbx_orb_glow.h"
#include "assets/kombify_rosette.h"
#include "c6_audio.h"
#include "c6_power.h"
#include "c6_voice.h"
#include "c6_wake.h"
#include "cloud_auth.h"
#include "display_design.h"
#include "net_wifi.h"
#include "ui_theme.h"

LV_FONT_DECLARE(kbx_lat_16);
LV_FONT_DECLARE(kbx_lat_20);
LV_FONT_DECLARE(kbx_lat_28);

static const char *TAG = "c6_ui";

/* Draw and DMA buffers, in screen lines (2 x 368 x lines x 2 B of internal
 * RAM). 32 lines left too little heap for a conversation plus the edge
 * check-in; the partial redraws of this UI do not need more. */
#define LVGL_BUF_LINES 20
#define SCREEN_W   368
#define SCREEN_H   448
#define STAGE_D    240
#define STAGE_CY   196
#define RING_D     196
#define CORE_D     156
#define HALO_MIN   200
#define HALO_MAX   236
#define REST_RIM   0x2A3346
#define CORE_HI    0x1F2737
#define CORE_LO    0x0C1119

/* Voice-state look, the values every board shares (ui_theme.h): at rest the
 * kombify mark keeps KBX_IDLE_MARK_SAT_PCT of its colour (recoloured toward
 * KBX_IDLE_MARK_GREY) at KBX_IDLE_MARK_BRIGHT_PCT brightness; every active
 * state (wake, connecting, listening, thinking, speaking) takes full colour
 * over KBX_STATE_FADE_MS. While a recognised or spoken sentence is shown the
 * orb sits behind KBX_REPLY_SCRIM_PCT of ground colour (here: its opacity)
 * and the sentence is drawn in front in the 28 px face (20 px when it is
 * long). No PSRAM here, so the grey is a runtime recolour of the mark
 * instead of a baked copy. */
#define IDLE_GREY_OPA    (255 * (100 - KBX_IDLE_MARK_SAT_PCT) / 100)   /* recolor strength at rest */
#define IDLE_MARK_OPA    (255 * KBX_IDLE_MARK_BRIGHT_PCT / 100)
#define BACK_STAGE_OPA   (255 * (100 - KBX_REPLY_SCRIM_PCT) / 100)     /* stage opacity behind the sentence */
#define ORB_FADE_MS      KBX_STATE_FADE_MS
#define SENTENCE_HOLD_MS 4000       /* the sentence stays this long after the turn */
#define SENTENCE_LONG    110        /* longer sentences use KBX_REPLY_FONT_FALLBACK_PX */
/* Quick settings: a downward swipe that starts in the top bezel strip. */
#define SWIPE_EDGE_PX    64
#define SWIPE_MIN_PX     56
#define TOUCH_H          56         /* button height, comfortably large */

/* Two designs (display_design.h, an account setting, fixed per boot):
 * classic is the orb stage with the brand line and the settings button above
 * it; watch is the Pixel Watch layout (Companion WatchAssistantScreen): the
 * Aura Orb is the whole face. A black rim of RIM_H at the top and the bottom
 * carries the secondary information (Wi-Fi, battery, time and the settings
 * button above; the state pill below); the orb sits between them with its
 * glow reaching under the rim. All watch layer sizes are the shared
 * proportions of aura_orb.c: glow 1.28, sweep ring 0.84, halo 0.80 .. 0.88,
 * core 0.68 and the mark 0.58 of the orb. */
#define W_RIM_H      64
#define W_STAGE_D    320
#define W_STAGE_CY   222                  /* centre of the area between the rims */
#define W_GLOW_D     410                  /* 1.28 x W_STAGE_D */
#define W_RING_D     268
#define W_CORE_D     218
#define W_HALO_MIN   256
#define W_HALO_MAX   282
#define W_HALO_STEP  6                    /* redraw only on a visible step */
#define SCRIM_OPA        (255 * KBX_REPLY_SCRIM_PCT / 100)   /* ground over the orb behind the sentence */
#define GLOW_OPA_REST    89                                  /* .35, as aura_orb.c */
#define GLOW_OPA_ALIVE   255
#define DESIGN_RESTART_MS 4000            /* idle time before a changed design restarts the box */

/* Aura state palette (speechkit.voice_ui.v1 tokens.json, as aura_orb.c). */
typedef enum { ORB_INACTIVE, ORB_CONNECTING, ORB_LISTENING, ORB_PROCESSING, ORB_SPEAKING, ORB_ERROR } orb_t;
static const uint32_t LEAD[] = { 0x9CA3B8, 0x38BDF8, 0x34D399, 0xFBBF24, 0x818CF8, 0xF87171 };
static const uint32_t TRAIL[] = { 0x64748B, 0x818CF8, 0x38BDF8, 0xF472B6, 0xA78BFA, 0xFB7185 };

typedef struct {
    const char *tap, *connect_account, *connecting, *listening, *thinking, *speaking;
    const char *no_wifi, *no_speech, *offline, *signin, *denied, *service, *failed;
    const char *pair_title, *pair_hint, *pair_wait, *paired, *pair_failed, *cancel;
    const char *volume, *language, *lang_name, *wifi, *wifi_none, *account, *account_on, *account_off;
    const char *disconnect, *connect, *done;
    const char *wake, *wake_off, *say_fmt;
    const char *power_off, *powering_off;
    const char *followups, *followups_off;
    const char *power_confirm;
    const char *companion, *busy;
} strings_t;

static const strings_t DE = {
    .tap = "Tippen zum Sprechen", .connect_account = "Tippen: Konto verbinden",
    .connecting = "Verbinde …", .listening = "Ich höre zu", .thinking = "Denke nach …",
    .speaking = "Antwortet", .no_wifi = "Kein WLAN", .no_speech = "Nichts gehört",
    .offline = "Keine Verbindung", .signin = "Bitte Konto neu verbinden", .denied = "Nicht freigeschaltet",
    .service = "Dienst nicht erreichbar", .failed = "Fehler",
    .pair_title = "Mit kombify verbinden", .pair_hint = "QR-Code mit dem Handy scannen\nund den Code bestätigen.",
    .pair_wait = "Anmeldung wird vorbereitet …", .paired = "Verbunden", .pair_failed = "Anmeldung fehlgeschlagen",
    .cancel = "Abbrechen", .volume = "Lautstärke", .language = "Sprache", .lang_name = "Deutsch",
    .wifi = "WLAN", .wifi_none = "nicht eingerichtet", .account = "Konto",
    .account_on = "verbunden", .account_off = "nicht verbunden", .disconnect = "Trennen",
    .connect = "Verbinden", .done = "Fertig",
    .power_off = "Ausschalten", .powering_off = "Wird ausgeschaltet.\nZum Einschalten kurz PWR dr\u00fccken.",
    .wake = "Weckwort", .wake_off = "Aus", .say_fmt = "Sag \"%s\" oder tippe",
    .followups = "Nachfragen", .followups_off = "Aus",
    .power_confirm = "Wirklich ausschalten?",
    .companion = "Frage den Companion …", .busy = "Anderes Gespräch läuft noch",
};

static const strings_t EN = {
    .tap = "Tap to talk", .connect_account = "Tap to connect your account",
    .connecting = "Connecting …", .listening = "Listening", .thinking = "Thinking …",
    .speaking = "Answering", .no_wifi = "No Wi-Fi", .no_speech = "Didn't hear anything",
    .offline = "No connection", .signin = "Please reconnect your account", .denied = "Not enabled",
    .service = "Service unavailable", .failed = "Error",
    .pair_title = "Connect to kombify", .pair_hint = "Scan the QR code with your phone\nand confirm the code.",
    .pair_wait = "Preparing sign-in …", .paired = "Connected", .pair_failed = "Sign-in failed",
    .cancel = "Cancel", .volume = "Volume", .language = "Language", .lang_name = "English",
    .wifi = "Wi-Fi", .wifi_none = "not set up", .account = "Account",
    .account_on = "connected", .account_off = "not connected", .disconnect = "Disconnect",
    .connect = "Connect", .done = "Done",
    .power_off = "Power off", .powering_off = "Powering off.\nPress PWR briefly to turn on.",
    .wake = "Wake word", .wake_off = "Off", .say_fmt = "Say \"%s\" or tap",
    .followups = "Follow-ups", .followups_off = "Off",
    .power_confirm = "Power off now?",
    .companion = "Asking Companion …", .busy = "Another conversation running",
};

static const strings_t *S(void)
{
    return strncmp(c6_voice_locale(), "de", 2) == 0 ? &DE : &EN;
}

static lv_obj_t *s_halo, *s_ring, *s_core, *s_mark, *s_stage, *s_say, *s_pill, *s_pill_label, *s_wifi_icon;
static lv_obj_t *s_glow, *s_scrim, *s_time, *s_bat;       /* watch design only */
static lv_obj_t *s_confirm, *s_confirm_title;            /* power-off confirmation */
static lv_obj_t *s_confirm_ok_label, *s_confirm_cancel_label;
static bool s_watch;                                     /* the watch design runs */
static uint32_t s_design_idle_since_ms;
static lv_obj_t *s_pair_sheet, *s_pair_title, *s_pair_qr, *s_pair_code, *s_pair_hint, *s_pair_btn_label;
static lv_obj_t *s_settings, *s_set_body, *s_set_done;
static orb_t s_orb = (orb_t)-1;
static bool s_spinning;
static int s_halo_d;
static uint32_t s_caption_seq = UINT32_MAX;
static int32_t s_act;                  /* 0 idle .. 255 active, animated */
static int32_t s_back;                 /* 0 orb in front .. 255 behind a sentence, animated */
static bool s_say_on;
static uint32_t s_idle_since_ms;
static lv_point_t s_press;             /* touch-down point (swipe start) */
static bool s_swipe_armed;
static const char *s_pill_text;
static char s_pill_buf[64];
static char s_pill_last[64];
static bool s_pair_open;
static char s_pair_uri[256];
static int64_t s_paired_at;

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* Centres an orb layer on its stage: the classic stage is the orb's own box,
 * the watch stage is the whole screen and the orb sits between the rims. */
static void on_stage(lv_obj_t *o)
{
    lv_obj_align(o, LV_ALIGN_CENTER, 0, s_watch ? W_STAGE_CY - SCREEN_H / 2 : 0);
}

static lv_obj_t *circle(lv_obj_t *parent, int d)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    on_stage(o);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 200, TOUCH_H);
    lv_obj_set_style_radius(b, TOUCH_H / 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? KBX_COL_BRAND : KBX_COL_SURF2), 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(KBX_COL_LINE), 0);
    lv_obj_t *l = label(b, &kbx_lat_20, primary ? KBX_COL_BG : KBX_COL_TEXT_HI);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (label_out) *label_out = l;
    return b;
}

/* ── Orb ─────────────────────────────────────────────────────────────── */

static void spin_cb(void *obj, int32_t v)
{
    lv_arc_set_rotation(obj, (int32_t)v);
}

static int32_t lerp_i(int32_t a, int32_t b, int32_t t)
{
    return a + (b - a) * t / 255;
}

static bool s_mark_error;

/* Applies the idle grey and the sentence dimming from s_act / s_back. */
static void orb_look(void)
{
    if (s_watch) {
        /* A ground-coloured scrim over the orb, not a group opacity: a faded
         * stage would render through a full-width layer buffer, and the C6
         * has no RAM to spare for one. */
        lv_obj_set_style_bg_opa(s_scrim, (lv_opa_t)lerp_i(0, SCRIM_OPA, s_back), 0);
    } else {
        lv_obj_set_style_opa(s_stage, (lv_opa_t)lerp_i(255, BACK_STAGE_OPA, s_back), 0);
    }
    lv_obj_set_style_image_recolor(s_mark, lv_color_hex(KBX_IDLE_MARK_GREY), 0);
    lv_obj_set_style_image_recolor_opa(s_mark, (lv_opa_t)lerp_i(IDLE_GREY_OPA, 0, s_act), 0);
    int32_t base = s_mark_error ? 140 : LV_OPA_COVER;
    lv_obj_set_style_image_opa(s_mark, (lv_opa_t)(lerp_i(IDLE_MARK_OPA, 255, s_act) * base / 255), 0);
}

static void act_cb(void *obj, int32_t v)
{
    (void)obj;
    s_act = v;
    orb_look();
}

static void back_cb(void *obj, int32_t v)
{
    (void)obj;
    s_back = v;
    orb_look();
    /* The sentence fades in as the orb goes back. */
    lv_obj_set_style_text_opa(s_say, (lv_opa_t)v, 0);
    if (v == 0) lv_obj_add_flag(s_say, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_say, LV_OBJ_FLAG_HIDDEN);
}

static void fade_to(lv_anim_exec_xcb_t cb, int32_t from, int32_t to)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, (void *)cb);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ORB_FADE_MS);
    lv_anim_start(&a);
}

static void orb_active(bool active)
{
    static int8_t shown = -1;
    if (shown == (int8_t)active) return;
    shown = (int8_t)active;
    lv_anim_delete((void *)act_cb, act_cb);
    fade_to(act_cb, s_act, active ? 255 : 0);
}

static void sentence_show(bool on)
{
    if (on == s_say_on) return;
    s_say_on = on;
    lv_anim_delete((void *)back_cb, back_cb);
    fade_to(back_cb, s_back, on ? 255 : 0);
}

static void orb_set(orb_t orb)
{
    if (orb == s_orb) return;
    s_orb = orb;
    lv_color_t lead = lv_color_hex(LEAD[orb]);
    bool spin = orb == ORB_CONNECTING || orb == ORB_PROCESSING;
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(orb == ORB_INACTIVE ? REST_RIM : TRAIL[orb]), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lead, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, orb == ORB_INACTIVE ? LV_OPA_40 : LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, orb == ORB_INACTIVE ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    lv_arc_set_angles(s_ring, 0, spin ? 100 : 360);
    if (spin && !s_spinning) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_ring);
        lv_anim_set_exec_cb(&a, spin_cb);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_duration(&a, orb == ORB_CONNECTING ? 1100 : 1600);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else if (!spin && s_spinning) {
        lv_anim_delete(s_ring, spin_cb);
        lv_arc_set_rotation(s_ring, 270);
    }
    s_spinning = spin;
    lv_obj_set_style_border_color(s_halo, lead, 0);
    lv_obj_set_style_border_opa(s_halo, orb == ORB_INACTIVE ? (s_watch ? LV_OPA_30 : LV_OPA_TRANSP) : LV_OPA_50, 0);
    if (s_glow) {
        lv_obj_set_style_image_recolor(s_glow, lead, 0);
        lv_obj_set_style_image_opa(s_glow, orb == ORB_INACTIVE ? GLOW_OPA_REST : GLOW_OPA_ALIVE, 0);
    }
    lv_obj_set_style_border_color(s_core, lv_color_hex(orb == ORB_INACTIVE ? REST_RIM : LEAD[orb]), 0);
    s_mark_error = orb == ORB_ERROR;
    orb_look();
    orb_active(orb != ORB_INACTIVE);
}

static void orb_level(uint8_t level)
{
    bool live = s_orb == ORB_LISTENING || s_orb == ORB_SPEAKING;
    const int lo = s_watch ? W_HALO_MIN : HALO_MIN, hi = s_watch ? W_HALO_MAX : HALO_MAX;
    int target = live ? lo + (hi - lo) * level / 100 : lo;
    /* Fast rise, slow release; redraw only on a visible step. */
    int d = target > s_halo_d ? target : s_halo_d - (s_halo_d - target + 3) / 4;
    d = s_watch ? lo + (d - lo) / W_HALO_STEP * W_HALO_STEP : (d & ~1);
    if (d == s_halo_d) return;
    s_halo_d = d;
    lv_obj_set_size(s_halo, d, d);
    on_stage(s_halo);
}

/* ── Pairing sheet ───────────────────────────────────────────────────── */

static void pair_close(void)
{
    s_pair_open = false;
    s_pair_uri[0] = '\0';
    lv_obj_add_flag(s_pair_sheet, LV_OBJ_FLAG_HIDDEN);
}

static void pair_cancel_cb(lv_event_t *e)
{
    (void)e;
    pair_close();
}

static void pair_open(void)
{
    const strings_t *s = S();
    s_pair_open = true;
    s_paired_at = 0;
    s_pair_uri[0] = '\0';
    lv_label_set_text(s_pair_title, s->pair_title);
    lv_label_set_text(s_pair_hint, s->pair_wait);
    lv_label_set_text(s_pair_code, "");
    lv_label_set_text(s_pair_btn_label, s->cancel);
    lv_obj_add_flag(s_pair_qr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pair_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_pair_sheet);
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    if (!st.pairing) {
        esp_err_t err = cloud_auth_start_pairing();
        ESP_LOGI(TAG, "pairing start: %s", esp_err_to_name(err));
    }
}

static void pair_tick(void)
{
    if (!s_pair_open) return;
    const strings_t *s = S();
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    if (st.paired && !st.pairing) {
        if (!s_paired_at) {
            s_paired_at = lv_tick_get();
            lv_obj_add_flag(s_pair_qr, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_pair_code, s->paired);
            lv_label_set_text(s_pair_hint, "");
            lv_label_set_text(s_pair_btn_label, s->done);
        } else if (lv_tick_elaps((uint32_t)s_paired_at) > 2500) {
            pair_close();
        }
        return;
    }
    if (st.pairing && st.verification_uri_complete[0] && strcmp(st.verification_uri_complete, s_pair_uri)) {
        strlcpy(s_pair_uri, st.verification_uri_complete, sizeof(s_pair_uri));
        lv_qrcode_update(s_pair_qr, s_pair_uri, strlen(s_pair_uri));
        lv_obj_remove_flag(s_pair_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_pair_code, st.user_code);
        lv_label_set_text(s_pair_hint, s->pair_hint);
    } else if (!st.pairing && !st.paired && st.error_code[0]) {
        lv_obj_add_flag(s_pair_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_pair_code, s->pair_failed);
        lv_label_set_text_fmt(s_pair_hint, "%s", st.error_code);
    }
}

static void build_pair_sheet(lv_obj_t *scr)
{
    s_pair_sheet = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pair_sheet);
    lv_obj_set_size(s_pair_sheet, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_pair_sheet, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_pair_sheet, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_pair_sheet, LV_OBJ_FLAG_SCROLLABLE);
    s_pair_title = label(s_pair_sheet, &kbx_lat_20, KBX_COL_TEXT_HI);
    lv_obj_align(s_pair_title, LV_ALIGN_TOP_MID, 0, 26);
    s_pair_qr = lv_qrcode_create(s_pair_sheet);
    lv_qrcode_set_size(s_pair_qr, 188);
    lv_qrcode_set_dark_color(s_pair_qr, lv_color_hex(KBX_COL_BG));
    lv_qrcode_set_light_color(s_pair_qr, lv_color_white());
    lv_obj_set_style_border_color(s_pair_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_pair_qr, 8, 0);
    lv_obj_align(s_pair_qr, LV_ALIGN_TOP_MID, 0, 66);
    s_pair_code = label(s_pair_sheet, &lv_font_montserrat_28, KBX_COL_BRAND);
    lv_obj_align(s_pair_code, LV_ALIGN_TOP_MID, 0, 278);
    s_pair_hint = label(s_pair_sheet, &kbx_lat_16, KBX_COL_TEXT_LO);
    lv_obj_set_width(s_pair_hint, 320);
    lv_obj_set_style_text_align(s_pair_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pair_hint, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_t *b = button(s_pair_sheet, "", false, pair_cancel_cb, &s_pair_btn_label);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_add_flag(s_pair_sheet, LV_OBJ_FLAG_HIDDEN);
}

/* ── Settings sheet ──────────────────────────────────────────────────── */

static void settings_build_body(void);

static void settings_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void volume_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    c6_audio_set_volume((int)lv_slider_get_value(slider));
}

static void language_cb(lv_event_t *e)
{
    (void)e;
    c6_voice_set_locale(strncmp(c6_voice_locale(), "de", 2) == 0 ? "en-US" : "de-DE");
    s_pill_text = NULL;
    settings_build_body();
}

static void wake_cb(lv_event_t *e)
{
    (void)e;
    /* Off -> Kombify -> Hey Kombify -> Hey Kubi -> Hey Jarvis -> Off */
    int next = (int)c6_wake_phrase() + 1;
    c6_wake_set_phrase(next >= C6_WAKE_COUNT ? C6_WAKE_OFF : (c6_wake_phrase_t)next);
    settings_build_body();
}

static void followups_cb(lv_event_t *e)
{
    (void)e;
    /* Off -> 1 -> 2 -> 3 -> Off */
    unsigned next = c6_voice_max_followups() + 1;
    c6_voice_set_max_followups(next > 3 ? 0 : next);
    settings_build_body();
}

static void power_off_timer(lv_timer_t *t)
{
    (void)t;
    c6_power_off();
}

/* The last row of the list asks first; only the dialog's own "Power off"
 * cuts the rails. */
static void power_confirm_open_cb(lv_event_t *e)
{
    (void)e;
    const strings_t *s = S();
    lv_label_set_text(s_confirm_title, s->power_confirm);
    lv_label_set_text(s_confirm_ok_label, s->power_off);
    lv_label_set_text(s_confirm_cancel_label, s->cancel);
    lv_obj_remove_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_confirm);
}

/* Cancel only closes the dialog: back in the menu, nothing else happened. */
static void power_confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void power_off_cb(lv_event_t *e)
{
    (void)e;
    const strings_t *s = S();
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_set_body);
    lv_obj_t *l = label(s_set_body, &kbx_lat_20, KBX_COL_TEXT_HI);
    lv_obj_set_width(l, 320);
    lv_label_set_text(l, s->powering_off);
    lv_obj_add_flag(lv_obj_get_parent(s_set_done), LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(power_off_timer, 1500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void account_cb(lv_event_t *e)
{
    (void)e;
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    if (st.paired) {
        cloud_auth_forget();
        settings_build_body();
    } else {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        pair_open();
    }
}

static lv_obj_t *settings_row(const char *name, const char *value)
{
    lv_obj_t *row = plain(s_set_body);
    lv_obj_set_size(row, 320, LV_SIZE_CONTENT);
    lv_obj_t *n = label(row, &kbx_lat_16, KBX_COL_TEXT_LO);
    lv_label_set_text(n, name);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
    if (value) {
        lv_obj_t *v = label(row, &kbx_lat_20, KBX_COL_TEXT_HI);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_width(v, 320);
        lv_label_set_text(v, value);
        lv_obj_align(v, LV_ALIGN_TOP_LEFT, 0, 22);
    }
    return row;
}

static void settings_build_body(void)
{
    const strings_t *s = S();
    lv_label_set_text(s_set_done, s->done);
    lv_obj_clean(s_set_body);
    lv_obj_t *row = settings_row(s->volume, NULL);
    lv_obj_set_height(row, 64);
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 296, 16);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 12, 38);
    /* A thumb-sized target around the thin track and a larger knob. */
    lv_obj_set_ext_click_area(slider, 20);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, c6_audio_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(KBX_COL_SURF2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(KBX_COL_BRAND), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(KBX_COL_TEXT_HI), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, volume_cb, LV_EVENT_RELEASED, NULL);

    char wake[48];
    c6_wake_phrase_t phrase = c6_wake_phrase();
    snprintf(wake, sizeof(wake), "%s: %s", s->wake, phrase == C6_WAKE_OFF ? s->wake_off : c6_wake_phrase_name(phrase));
    lv_obj_t *wk = button(s_set_body, wake, false, wake_cb, NULL);
    lv_obj_set_width(wk, 320);

    char fu[48];
    unsigned fus = c6_voice_max_followups();
    if (fus) snprintf(fu, sizeof(fu), "%s: %u", s->followups, fus);
    else snprintf(fu, sizeof(fu), "%s: %s", s->followups, s->followups_off);
    lv_obj_t *fub = button(s_set_body, fu, false, followups_cb, NULL);
    lv_obj_set_width(fub, 320);

    lv_obj_t *lang = button(s_set_body, s->lang_name, false, language_cb, NULL);
    lv_obj_set_width(lang, 320);

    char wifi[80];
    char ip[16] = "";
    const char *ssid = net_wifi_ssid();
    bool up = net_wifi_is_connected(ip, sizeof(ip));
    if (!ssid || !ssid[0]) snprintf(wifi, sizeof(wifi), "%s", s->wifi_none);
    else snprintf(wifi, sizeof(wifi), "%s  %s", ssid, up ? ip : "…");
    settings_row(s->wifi, wifi);

    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    settings_row(s->account, st.paired ? s->account_on : s->account_off);
    lv_obj_t *acc = button(s_set_body, st.paired ? s->disconnect : s->connect, !st.paired, account_cb, NULL);
    lv_obj_set_width(acc, 320);


    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *ver = label(s_set_body, &kbx_lat_16, KBX_COL_TEXT_LO);
    lv_label_set_text_fmt(ver, "kombify box C6 - %s - %s", app->version, bsp_board_variant_to_name(bsp_board_get_variant()));

    /* Power off is the last row of the list, a destructive row (alert colours),
     * never next to Done; it asks for confirmation first. */
    lv_obj_t *off = button(s_set_body, s->power_off, false, power_confirm_open_cb, NULL);
    lv_obj_set_width(off, 320);
    lv_obj_set_style_border_color(off, lv_color_hex(KBX_COL_ALERT_LINE), 0);
    lv_obj_set_style_bg_color(off, lv_color_hex(KBX_COL_ALERT_BG), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(off, 0), lv_color_hex(KBX_COL_ALERT), 0);
}

static void settings_open_cb(lv_event_t *e)
{
    (void)e;
    settings_build_body();
    lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings);
}

static void build_settings(lv_obj_t *scr)
{
    s_settings = lv_obj_create(scr);
    lv_obj_remove_style_all(s_settings);
    lv_obj_set_size(s_settings, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_settings, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);
    s_set_body = plain(s_settings);
    lv_obj_set_size(s_set_body, 320, SCREEN_H - 22 - TOUCH_H - 32);
    lv_obj_align(s_set_body, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_flex_flow(s_set_body, LV_FLEX_FLOW_COLUMN);
    /* More rows than fit on the panel: the body scrolls under Done. */
    lv_obj_add_flag(s_set_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_set_body, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_set_body, 12, 0);
    /* The bottom row keeps only the close action, always visible. */
    lv_obj_t *done = button(s_settings, "", true, settings_close_cb, &s_set_done);
    lv_obj_set_width(done, 320);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
}

/* The power-off confirmation: a sheet over the settings with the question,
 * "Power off" (destructive) and "Cancel" (back to the menu). */
static void build_power_confirm(lv_obj_t *scr)
{
    s_confirm = lv_obj_create(scr);
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_size(s_confirm, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_confirm, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_confirm, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_confirm, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_confirm, 16, 0);
    s_confirm_title = label(s_confirm, &kbx_lat_28, KBX_COL_TEXT_HI);
    lv_obj_set_width(s_confirm_title, 320);
    lv_obj_set_style_text_align(s_confirm_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *ok = button(s_confirm, "", false, power_off_cb, &s_confirm_ok_label);
    lv_obj_set_width(ok, 280);
    lv_obj_set_style_border_color(ok, lv_color_hex(KBX_COL_ALERT_LINE), 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(KBX_COL_ALERT_BG), 0);
    lv_obj_set_style_text_color(s_confirm_ok_label, lv_color_hex(KBX_COL_ALERT), 0);
    lv_obj_t *cancel = button(s_confirm, "", true, power_confirm_cancel_cb, &s_confirm_cancel_label);
    lv_obj_set_width(cancel, 280);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}

/* ── Main scene ──────────────────────────────────────────────────────── */

static void orb_tap_cb(lv_event_t *e)
{
    (void)e;
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    if (!st.paired && c6_voice_state() == C6_VOICE_IDLE) {
        pair_open();
        return;
    }
    c6_voice_tap();
}

static void pill_set(const char *text)
{
    if (s_pill_text && !strcmp(text, s_pill_last)) return;
    s_pill_text = text;
    strlcpy(s_pill_last, text, sizeof(s_pill_last));
    lv_label_set_text(s_pill_label, text);
}

static const char *error_text(const strings_t *s, const char *code)
{
    if (strstr(code, "no_speech")) return s->no_speech;
    if (strstr(code, "wifi") || strstr(code, "connect")) return s->offline;
    if (strstr(code, "signed_out") || strstr(code, "signin") || strstr(code, "401")) return s->signin;
    if (strstr(code, "per_user_limit")) return s->busy;
    if (strstr(code, "403") || strstr(code, "402") || strstr(code, "429")) return s->denied;
    return NULL;
}

/* AMOLED: a static screen burns in and lights the room. After 2 min
 * without touch while idle the panel goes dark; a touch or any voice turn
 * (wake word, KBX talk) lights it again. The touch that wakes the screen is
 * swallowed by a full-screen catcher so it never starts a turn. */
#define SCREEN_TIMEOUT_MS (2 * 60 * 1000)
static lv_obj_t *s_wake_catcher;
static bool s_screen_off;

static void screen_set(bool on)
{
    if (on == !s_screen_off) return;
    s_screen_off = !on;
    bsp_display_brightness_set(on ? 80 : 0);
    if (on) lv_obj_add_flag(s_wake_catcher, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_wake_catcher, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off");
}

static void wake_catcher_cb(lv_event_t *e)
{
    (void)e;
    screen_set(true);
    lv_display_trigger_activity(NULL);
}

/* Terminator length at p (ASCII, ellipsis, ideographic and Arabic marks), 0 when p is none. */
static int sentence_term_len(const char *p)
{
    unsigned char c = (unsigned char)p[0];
    if (c == '.' || c == '!' || c == '?') return 1;
    if (c == 0xD8 && (unsigned char)p[1] == 0x9F) return 2;                                 /* U+061F */
    if (c == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0xA6) return 3;  /* U+2026 */
    if (c == 0xE3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x82) return 3;  /* U+3002 */
    if (c == 0xEF && (unsigned char)p[1] == 0xBC &&
        ((unsigned char)p[2] == 0x9F || (unsigned char)p[2] == 0x81)) return 3;             /* U+FF1F, U+FF01 */
    return 0;
}

/* Keeps only the last sentence of an accumulated transcript: the overlay
 * shows the current sentence, never a log (same rule as the S3 boards,
 * firmware/components/kbx_ui/ui_scene.c last_sentence). */
static void last_sentence(char *text)
{
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\n')) text[--n] = '\0';
    const char *start = text;
    for (const char *p = text; *p;) {
        int t = sentence_term_len(p);
        if (!t) {
            p++;
            continue;
        }
        const char *q = p + t;
        while (sentence_term_len(q)) q += sentence_term_len(q);   /* "?!", "..." */
        if (*q == '\0') break;                                    /* trailing terminator */
        if (*q == ' ' || *q == '\n' || (unsigned char)*p >= 0xE3) {
            while (*q == ' ' || *q == '\n') q++;
            if (*q) start = q;
        }
        p = q;
    }
    if (start != text) memmove(text, start, strlen(start) + 1);
}

/* ── Watch design rim: time and battery ──────────────────────────────── */

#define BAT_POLL_MS 30000

/* AXP2101 fuel gauge: percent, or -1 without a battery. Added and removed per
 * read like c6_power.c does, so no PMU handle is held between reads. */
static int battery_read(bool *charging)
{
    i2c_master_dev_handle_t pmu = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_PMU_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &pmu) != ESP_OK) return -1;
    uint8_t status = 0, charge = 0, pct = 0;
    const uint8_t r_status = 0x00, r_charge = 0x01, r_pct = 0xA4;
    int out = -1;
    if (i2c_master_transmit_receive(pmu, &r_status, 1, &status, 1, 50) == ESP_OK && (status & 0x08) &&
        i2c_master_transmit_receive(pmu, &r_pct, 1, &pct, 1, 50) == ESP_OK && pct <= 100) {
        out = pct;
        *charging = i2c_master_transmit_receive(pmu, &r_charge, 1, &charge, 1, 50) == ESP_OK &&
                    ((charge >> 5) & 0x03) == 0x01;
    }
    i2c_master_bus_rm_device(pmu);
    return out;
}

/* Central European time, the S3 boards' default region (ui_scene.c
 * KBX_DEFAULT_TZ): UTC+1, +2 from 01:00 UTC on the last Sunday of March to
 * 01:00 UTC on the last Sunday of October. Computed here instead of through
 * setenv/tzset, which would keep libc timezone state in the C6's scarce heap. */
static void local_time(time_t utc, struct tm *tm)
{
    gmtime_r(&utc, tm);
    const int y = tm->tm_year + 1900, mon = tm->tm_mon + 1;
    const int last_sun_mar = 31 - (5 * y / 4 + 4) % 7;
    const int last_sun_oct = 31 - (5 * y / 4 + 1) % 7;
    const bool dst = (mon > 3 && mon < 10) ||
                     (mon == 3 && (tm->tm_mday > last_sun_mar || (tm->tm_mday == last_sun_mar && tm->tm_hour >= 1))) ||
                     (mon == 10 && (tm->tm_mday < last_sun_oct || (tm->tm_mday == last_sun_oct && tm->tm_hour < 1)));
    utc += dst ? 7200 : 3600;
    gmtime_r(&utc, tm);
}

/* The time (once SNTP set the clock) and the battery, in the top rim. */
static void rim_tick(void)
{
    static char time_shown[8] = "?";
    char hhmm[8] = "";
    time_t now = time(NULL);
    if (now >= 1700000000) {
        struct tm tm;
        local_time(now, &tm);
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    if (strcmp(hhmm, time_shown)) {
        strlcpy(time_shown, hhmm, sizeof(time_shown));
        lv_label_set_text(s_time, hhmm);
    }

    static uint32_t polled_ms;
    static int pct = -2;
    static bool charging;
    uint32_t now_ms = lv_tick_get();
    if (pct == -2 || now_ms - polled_ms >= BAT_POLL_MS) {
        polled_ms = now_ms;
        bool c = false;
        int p = battery_read(&c);
        if (p != pct || c != charging) {
            pct = p;
            charging = c;
            if (p < 0) {
                lv_label_set_text(s_bat, "");
            } else {
                const char *sym = p >= 85 ? LV_SYMBOL_BATTERY_FULL : p >= 60 ? LV_SYMBOL_BATTERY_3
                                : p >= 35 ? LV_SYMBOL_BATTERY_2 : p >= 12 ? LV_SYMBOL_BATTERY_1
                                : LV_SYMBOL_BATTERY_EMPTY;
                lv_label_set_text_fmt(s_bat, "%s%s %d%%", charging ? LV_SYMBOL_CHARGE " " : "", sym, p);
                lv_obj_set_style_text_color(s_bat, lv_color_hex(p < 15 && !charging ? KBX_COL_ALERT : KBX_COL_TEXT_MID), 0);
            }
        }
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    const strings_t *s = S();
    c6_voice_state_t vs = c6_voice_state();
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    bool wifi = net_wifi_is_connected(NULL, 0);

    static const orb_t MAP[] = {
        [C6_VOICE_IDLE] = ORB_INACTIVE, [C6_VOICE_CONNECTING] = ORB_CONNECTING,
        [C6_VOICE_LISTENING] = ORB_LISTENING, [C6_VOICE_THINKING] = ORB_PROCESSING,
        [C6_VOICE_SPEAKING] = ORB_SPEAKING, [C6_VOICE_ERROR] = ORB_ERROR,
    };
    orb_set(MAP[vs]);
    orb_level(c6_audio_level());

    switch (vs) {
    case C6_VOICE_CONNECTING: pill_set(s->connecting); break;
    case C6_VOICE_LISTENING: pill_set(s->listening); break;
    case C6_VOICE_THINKING: pill_set(c6_voice_companion() ? s->companion : s->thinking); break;
    case C6_VOICE_SPEAKING: pill_set(s->speaking); break;
    case C6_VOICE_ERROR: {
        char code[48];
        c6_voice_error(code, sizeof(code));
        const char *known = error_text(s, code);
        if (known) {
            pill_set(known);
        } else {
            snprintf(s_pill_buf, sizeof(s_pill_buf), "%s: %s", s->failed, code);
            s_pill_text = NULL;
            pill_set(s_pill_buf);
        }
        break;
    }
    case C6_VOICE_IDLE:
    default:
        if (wifi && st.paired && c6_wake_listening()) {
            snprintf(s_pill_buf, sizeof(s_pill_buf), s->say_fmt, c6_wake_phrase_name(c6_wake_phrase()));
            pill_set(s_pill_buf);
        } else {
            pill_set(!wifi ? s->no_wifi : st.paired ? s->tap : s->connect_account);
        }
        break;
    }

    /* The current sentence (what the user said, then what the assistant
     * says) comes to the front in a large font while the orb moves back; it
     * stays a few seconds after the turn, then the orb returns. */
    char caption[256];
    bool agent = false;
    uint32_t seq = 0;
    bool has = c6_voice_caption(caption, sizeof(caption), &agent, &seq);
    if (seq != s_caption_seq) {
        s_caption_seq = seq;
        last_sentence(caption);
        lv_obj_set_style_text_font(s_say, strlen(caption) > SENTENCE_LONG ? &kbx_lat_20 : &kbx_lat_28, 0);
        lv_obj_set_style_text_color(s_say, lv_color_hex(agent ? KBX_COL_TEXT_HI : KBX_COL_TEXT_MID), 0);
        lv_label_set_text(s_say, caption);
    }
    uint32_t now_ms = lv_tick_get();
    if (vs != C6_VOICE_IDLE) s_idle_since_ms = 0;
    else if (!s_idle_since_ms) s_idle_since_ms = now_ms | 1U;
    bool hold = vs != C6_VOICE_IDLE || now_ms - s_idle_since_ms < SENTENCE_HOLD_MS;
    sentence_show(has && hold);

    static int wifi_shown = -1;
    if (wifi_shown != (int)wifi) {
        wifi_shown = wifi;
        lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(wifi ? KBX_COL_BRAND : KBX_COL_ALERT), 0);
    }
    if (s_watch) rim_tick();
    pair_tick();

    /* A design chosen in the settings takes effect through a clean restart,
     * once the box has been idle for a few seconds (the applied settings
     * revision is stored by then). */
    if (kbx_design_restart_needed()) {
        if (vs != C6_VOICE_IDLE || s_pair_open) s_design_idle_since_ms = 0;
        else if (!s_design_idle_since_ms) s_design_idle_since_ms = lv_tick_get() | 1U;
        else if (lv_tick_elaps(s_design_idle_since_ms) > DESIGN_RESTART_MS) {
            ESP_LOGW(TAG, "display design %s: restarting", kbx_design_id(kbx_design_stored()));
            esp_restart();
        }
    }

    bool busy = vs != C6_VOICE_IDLE || s_pair_open;
    if (busy) {
        if (s_screen_off) lv_display_trigger_activity(NULL);
        screen_set(true);
    } else if (!s_screen_off && lv_display_get_inactive_time(NULL) > SCREEN_TIMEOUT_MS) {
        screen_set(false);
    }
}

static void build_scene_classic(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = label(scr, &lv_font_montserrat_20, KBX_COL_BRAND);
    lv_label_set_text(brand, "kombify");
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 22, 22);

    s_wifi_icon = label(scr, &lv_font_montserrat_20, KBX_COL_TEXT_LO);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_icon, LV_ALIGN_TOP_RIGHT, -76, 22);

    lv_obj_t *gear = lv_button_create(scr);
    lv_obj_set_size(gear, TOUCH_H, TOUCH_H);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(KBX_COL_SURF), 0);
    lv_obj_set_style_border_color(gear, lv_color_hex(KBX_COL_LINE), 0);
    lv_obj_set_style_border_width(gear, 1, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_t *gl = label(gear, &lv_font_montserrat_20, KBX_COL_TEXT_MID);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gl);
    lv_obj_add_event_cb(gear, settings_open_cb, LV_EVENT_CLICKED, NULL);

    /* Grab handle: the quick settings open with a swipe down from here. */
    lv_obj_t *grab = plain(scr);
    lv_obj_set_size(grab, 44, 4);
    lv_obj_set_style_radius(grab, 2, 0);
    lv_obj_set_style_bg_color(grab, lv_color_hex(KBX_COL_TEXT_LO), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_40, 0);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *stage = plain(scr);
    s_stage = stage;
    lv_obj_set_size(stage, STAGE_D, STAGE_D);
    lv_obj_align(stage, LV_ALIGN_TOP_MID, 0, STAGE_CY - STAGE_D / 2);
    lv_obj_add_flag(stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stage, orb_tap_cb, LV_EVENT_CLICKED, NULL);

    s_halo_d = HALO_MIN;
    s_halo = circle(stage, HALO_MIN);
    lv_obj_set_style_border_width(s_halo, 2, 0);

    s_ring = lv_arc_create(stage);
    lv_obj_set_size(s_ring, RING_D, RING_D);
    lv_obj_center(s_ring);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_rotation(s_ring, 270);
    lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);

    s_core = circle(stage, CORE_D);
    lv_obj_set_style_bg_opa(s_core, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_core, lv_color_hex(CORE_HI), 0);
    lv_obj_set_style_bg_grad_color(s_core, lv_color_hex(CORE_LO), 0);
    lv_obj_set_style_bg_grad_dir(s_core, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(s_core, 1, 0);

    s_mark = lv_image_create(stage);
    lv_image_set_src(s_mark, &kombify_rosette_104);
    lv_obj_remove_flag(s_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_mark);

    /* The sentence layer: one label, centred in front of the orb. */
    lv_obj_t *say_box = plain(scr);
    lv_obj_set_size(say_box, 336, 300);
    lv_obj_align(say_box, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_flex_flow(say_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(say_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_say = label(say_box, &kbx_lat_28, KBX_COL_TEXT_HI);
    lv_obj_set_width(s_say, 324);
    lv_obj_set_style_text_align(s_say, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s_say, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_say, "");
    lv_obj_add_flag(s_say, LV_OBJ_FLAG_HIDDEN);

    s_pill = plain(scr);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_min_width(s_pill, 180, 0);
    lv_obj_set_style_pad_hor(s_pill, 20, 0);
    lv_obj_set_style_radius(s_pill, 20, 0);
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(KBX_COL_PILL), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_pill, lv_color_hex(KBX_COL_LINE), 0);
    lv_obj_set_style_border_width(s_pill, 1, 0);
    lv_obj_align(s_pill, LV_ALIGN_BOTTOM_MID, 0, -18);
    s_pill_label = label(s_pill, &kbx_lat_16, KBX_COL_TEXT_MID);
    lv_obj_center(s_pill_label);
    lv_label_set_text(s_pill_label, "");
}

static void build_scene_watch(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* The orb: one full-screen layer root so the glow can reach under the
     * rims; the tap target and the rim widgets sit above it. */
    lv_obj_t *stage = plain(scr);
    s_stage = stage;
    lv_obj_set_size(stage, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(stage, 0, 0);
    /* The whole face is the talk target, as on the watch; the settings
     * button and the sheets sit above it. */
    lv_obj_add_flag(stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stage, orb_tap_cb, LV_EVENT_CLICKED, NULL);

    s_glow = lv_image_create(stage);
    lv_image_set_src(s_glow, &kbx_orb_glow);
    lv_image_set_antialias(s_glow, true);
    lv_image_set_scale(s_glow, (uint32_t)(256 * W_GLOW_D / kbx_orb_glow.header.w));
    lv_obj_remove_flag(s_glow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_image_recolor_opa(s_glow, LV_OPA_COVER, 0);
    on_stage(s_glow);

    s_halo_d = W_HALO_MIN;
    s_halo = circle(stage, W_HALO_MIN);
    lv_obj_set_style_border_width(s_halo, 2, 0);

    s_ring = lv_arc_create(stage);
    lv_obj_set_size(s_ring, W_RING_D, W_RING_D);
    on_stage(s_ring);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_rotation(s_ring, 270);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);

    s_core = circle(stage, W_CORE_D);
    lv_obj_set_style_bg_opa(s_core, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_core, lv_color_hex(CORE_HI), 0);
    lv_obj_set_style_bg_grad_color(s_core, lv_color_hex(CORE_LO), 0);
    lv_obj_set_style_bg_grad_dir(s_core, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(s_core, 1, 0);

    s_mark = lv_image_create(stage);
    lv_image_set_src(s_mark, &kombify_rosette_186);
    lv_obj_remove_flag(s_mark, LV_OBJ_FLAG_CLICKABLE);
    on_stage(s_mark);

    /* Reply overlay: the ground over the orb, faded in while a sentence shows. */
    s_scrim = plain(stage);
    lv_obj_set_size(s_scrim, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_TRANSP, 0);

    /* Top rim: Wi-Fi and battery left, the time in the middle, the settings
     * button right (the 56 px touch target of the quick settings). */
    s_wifi_icon = label(scr, &lv_font_montserrat_20, KBX_COL_TEXT_LO);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_icon, LV_ALIGN_TOP_LEFT, 22, 22);

    s_bat = label(scr, &lv_font_montserrat_16, KBX_COL_TEXT_MID);
    lv_label_set_text(s_bat, "");
    lv_obj_align_to(s_bat, s_wifi_icon, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    s_time = label(scr, &lv_font_montserrat_28, KBX_COL_TEXT_HI);
    lv_label_set_text(s_time, "");
    lv_obj_set_width(s_time, 100);
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *gear = lv_button_create(scr);
    lv_obj_set_size(gear, TOUCH_H, TOUCH_H);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(KBX_COL_SURF), 0);
    lv_obj_set_style_border_color(gear, lv_color_hex(KBX_COL_LINE), 0);
    lv_obj_set_style_border_width(gear, 1, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_t *gl = label(gear, &lv_font_montserrat_20, KBX_COL_TEXT_MID);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gl);
    lv_obj_add_event_cb(gear, settings_open_cb, LV_EVENT_CLICKED, NULL);

    /* Grab handle: the quick settings open with a swipe down from here. */
    lv_obj_t *grab = plain(scr);
    lv_obj_set_size(grab, 44, 4);
    lv_obj_set_style_radius(grab, 2, 0);
    lv_obj_set_style_bg_color(grab, lv_color_hex(KBX_COL_TEXT_LO), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_40, 0);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 6);

    /* The sentence layer: one label, centred in front of the orb. */
    lv_obj_t *say_box = plain(scr);
    lv_obj_set_size(say_box, 336, 300);
    on_stage(say_box);
    lv_obj_set_flex_flow(say_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(say_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_say = label(say_box, &kbx_lat_28, KBX_COL_TEXT_HI);
    lv_obj_set_width(s_say, 324);
    lv_obj_set_style_text_align(s_say, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s_say, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_say, "");
    lv_obj_add_flag(s_say, LV_OBJ_FLAG_HIDDEN);

    /* Bottom rim: the state pill. */
    s_pill = plain(scr);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_min_width(s_pill, 200, 0);
    lv_obj_set_style_pad_hor(s_pill, 22, 0);
    lv_obj_set_style_radius(s_pill, 22, 0);
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(KBX_COL_PILL), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_pill, lv_color_hex(KBX_COL_LINE), 0);
    lv_obj_set_style_border_width(s_pill, 1, 0);
    lv_obj_align(s_pill, LV_ALIGN_BOTTOM_MID, 0, -14);
    s_pill_label = label(s_pill, &kbx_lat_16, KBX_COL_TEXT_MID);
    lv_obj_center(s_pill_label);
    lv_label_set_text(s_pill_label, "");
}

static void build_scene(lv_obj_t *scr)
{
    if (s_watch) build_scene_watch(scr);
    else build_scene_classic(scr);
}

/* ── Quick settings: swipe down from the top edge ────────────────────── */

/* The corner button sits where the panel's touch is unreliable, so a swipe
 * down that starts in the top strip opens the settings as well (the round S3
 * board does the same from its top bezel, firmware/components/kbx_ui/touch.c).
 * LVGL reports press and release to the input device; the points in between
 * are polled. */
static bool settings_visible(void)
{
    return !lv_obj_has_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
}

static bool swipe_check(lv_point_t end)
{
    if (!s_swipe_armed || s_press.y > SWIPE_EDGE_PX) return false;
    int dy = end.y - s_press.y;
    int dx = end.x > s_press.x ? end.x - s_press.x : s_press.x - end.x;
    if (dy < SWIPE_MIN_PX || dy <= dx) return false;
    s_swipe_armed = false;
    if (settings_visible() || s_pair_open) return false;
    ESP_LOGI(TAG, "swipe down from the top edge (y=%d, travel %d px) -> settings", (int)s_press.y, dy);
    settings_open_cb(NULL);
    return true;
}

static void touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_target(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_press = p;
        s_swipe_armed = true;
    } else {
        (void)swipe_check(p);
        s_swipe_armed = false;
    }
}

static void swipe_poll_cb(lv_timer_t *t)
{
    (void)t;
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (!s_swipe_armed || !indev || lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    (void)swipe_check(p);
}

/* KBX swipe: the same decision with a synthetic stroke, for the serial log. */
void c6_ui_swipe_selftest(void)
{
    if (!bsp_display_lock(1000)) return;
    s_press = (lv_point_t){ SCREEN_W / 2, 10 };
    s_swipe_armed = true;
    bool opened = swipe_check((lv_point_t){ SCREEN_W / 2 + 12, 10 + SWIPE_MIN_PX + 8 });
    ESP_LOGI(TAG, "swipe selftest: %s", opened ? "settings opened" : "not opened");
    bsp_display_unlock();
}

/* ── Screenshot over USB (KBX shot) ──────────────────────────────────── */

static volatile bool s_shot;
static lv_display_t *s_disp;

/* Each flushed RGB565 band goes out as "#SHOT x1 y1 x2 y2 <base64>" before
 * the port swaps its bytes; tools/c6-screenshot.py reassembles a PNG. */
static size_t b64(unsigned char *out, const uint8_t *in, size_t len)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
            (i + 2 < len ? in[i + 2] : 0);
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < len ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? T[v & 63] : '=';
    }
    return o;
}

/* The driver's TX ring holds 256 bytes and refuses larger writes whole. */
static void usj_write(const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        size_t n = len > 128 ? 128 : len;
        int sent = usb_serial_jtag_write_bytes(p, n, portMAX_DELAY);
        if (sent <= 0) return;
        p += sent;
        len -= (size_t)sent;
    }
}

static void shot_flush_cb(lv_event_t *e)
{
    if (!s_shot) return;
    const lv_area_t *a = lv_event_get_param(e);
    lv_draw_buf_t *buf = lv_display_get_buf_active(s_disp);
    if (!a || !buf) return;
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    size_t row = (size_t)w * 2;
    char head[48];
    int n = snprintf(head, sizeof(head), "\n#SHOT %d %d %d %d ", (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2);
    usj_write(head, (size_t)n);
    static unsigned char enc[(SCREEN_W * 2 * 2 + 2) / 3 * 4 + 4];
    for (int y = 0; y < h; y += 2) {
        size_t len = row * (size_t)(y + 1 < h ? 2 : 1);
        size_t out = b64(enc, buf->data + (size_t)y * buf->header.stride, len);
        usj_write(enc, out);
        usj_write("|", 1);
    }
    usj_write("\n", 1);
}

static void shot_done_cb(lv_event_t *e)
{
    (void)e;
    if (!s_shot) return;
    s_shot = false;
    usj_write("\n#SHOT END\n", 11);
}

void c6_ui_screenshot(void)
{
    if (!bsp_display_lock(1000)) return;
    s_shot = true;
    lv_obj_invalidate(lv_screen_active());
    bsp_display_unlock();
}

esp_err_t c6_ui_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * LVGL_BUF_LINES,
        .trans_size = BSP_LCD_H_RES * LVGL_BUF_LINES * 2,
        .double_buffer = false,
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    cfg.lvgl_port_cfg.task_stack = 6144;
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) return ESP_FAIL;
    ESP_LOGI(TAG, "display %s started", bsp_board_variant_to_name(bsp_board_get_variant()));
    bsp_display_brightness_set(80);

    bsp_display_lock(0);
    s_disp = disp;
    lv_display_add_event_cb(disp, shot_flush_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, shot_done_cb, LV_EVENT_RENDER_READY, NULL);
    lv_obj_t *scr = lv_screen_active();
    kbx_design_init(KBX_DESIGN_WATCH);
    s_watch = kbx_design() == KBX_DESIGN_WATCH;
    ESP_LOGI(TAG, "display design %s", kbx_design_id(kbx_design()));
    build_scene(scr);
    build_pair_sheet(scr);
    build_settings(scr);
    build_power_confirm(scr);
    orb_set(ORB_INACTIVE);
    s_wake_catcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake_catcher);
    lv_obj_set_size(s_wake_catcher, SCREEN_W, SCREEN_H);
    lv_obj_add_flag(s_wake_catcher, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wake_catcher, wake_catcher_cb, LV_EVENT_PRESSED, NULL);
    lv_timer_create(tick_cb, 50, NULL);
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) {
        lv_indev_add_event_cb(indev, touch_cb, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, touch_cb, LV_EVENT_RELEASED, NULL);
        lv_timer_create(swipe_poll_cb, 25, NULL);
    }
    bsp_display_unlock();
    return ESP_OK;
}

void c6_ui_show_settings(void)
{
    if (!bsp_display_lock(1000)) return;
    settings_open_cb(NULL);
    bsp_display_unlock();
}

void c6_ui_show_settings_at(const char *where)
{
    if (!bsp_display_lock(1000)) return;
    settings_open_cb(NULL);
    if (where && !strcmp(where, "end")) {
        lv_obj_update_layout(s_set_body);
        lv_obj_scroll_to_y(s_set_body, LV_COORD_MAX, LV_ANIM_OFF);
    } else if (where && !strcmp(where, "confirm")) {
        power_confirm_open_cb(NULL);
    }
    bsp_display_unlock();
}

void c6_ui_show_pairing(void)
{
    if (!bsp_display_lock(1000)) return;
    pair_open();
    bsp_display_unlock();
}
