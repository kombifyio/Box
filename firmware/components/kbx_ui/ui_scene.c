/*
 * kombify box - UI 2 voice scene (mockup frames R-1, R-3 ... R-7, R-11).
 *
 * Two designs (display_design.h, an account setting; the running one is fixed
 * at start): "classic" and "watch" (the Pixel Watch design language: on the
 * RLCD two solid rims, ui_home_watch.c).
 *
 * Round face (both designs): the Aura Orb fills the display and its
 * coloured ring is the one status ring (state colour, level halo, red when
 * muted); there is no second edge ring. Time, chips, caption and the status
 * pill sit inside the ring, upright, around the centred kombify mark.
 *
 * Zones (display profile): stage (Aura Orb with the kombify rosette), top
 * chrome (time + Wi-Fi in idle, the mute or conversation chip otherwise),
 * caption (transcript or a one-line message), bottom status pill. The error
 * state (R-11) swaps caption and pill for one localized line, a retry pill
 * and the short reason code.
 *
 * Every LVGL mutation happens in the scene timer; values are compared with
 * what is drawn so an idle box does not redraw (QSPI budget).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio.h"
#include "aura_orb.h"
#include "battery.h"
#include "box_status.h"
#include "display_design.h"
#include "display_profile.h"
#include "esp_system.h"
#include "i18n.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "ui.h"
#include "ui_style.h"
#include "ui_theme.h"
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif
#if CONFIG_KBX_HOME_SCREEN
#include "ui_home.h"
#endif
/* The host preview has no sdkconfig and retains its round touch profile. */
#if !defined(ESP_PLATFORM) && !defined(CONFIG_KBX_BOARD_ID)
#define UI_HAS_TOUCH 1
#elif CONFIG_KBX_INPUT_TOUCH
#define UI_HAS_TOUCH 1
#else
#define UI_HAS_TOUCH 0
#endif
#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
/* Wide RLCD voice scene: a small orb top left, dimmed
 * behind the reply overlay, and the one current sentence (the user's or the
 * agent's, never a log) in a large bubble in front (28 px Latin, up to 6
 * lines). The user's bubble is solid ink with paper text and sits right, the
 * agent's is outlined and sits left; all text is solid ink so it reads from
 * across the room on the 1-bit panel. */
#define CAP_BUF           640
#define BUB_PAD_H         12
#define BUB_PAD_V         8
#define BUB_RADIUS        16
#define CUR_W             368
#define CUR_BOTTOM        252                 /* the bubble ends above the pill */
/* Classic: up to 6 lines in a bubble above the pill. Watch: the bubble is
 * centred between the rims (48 .. 252), up to 5 lines. */
static int s_cur_h = CUR_BOTTOM - 16;
static int s_max_lines = 6;                   /* the font steps down first */
#define CUR_H             s_cur_h
#define CAPTION_MAX_LINES s_max_lines
static lv_obj_t *s_cur_bub;
static int s_cap_kind = NET_WS_CAPTION_NONE;
#else
/* Round touch: the reply overlay is a centred label over the dimmed orb, as
 * many lines as fit the face inside the ring above the state pill: the block
 * is centred REPLY_RAISE px above the stage, so its 124 px end at y 228, above
 * the pill (top 237), and its corners stay inside the ring (r < 133). */
#define CAPTION_MAX_LINES 4
#define CAP_BUF           640   /* as long as the transcript the voice layer keeps (net_ws CAPTION_MAX 600) */
#define REPLY_H           124
#define REPLY_RAISE       14
static lv_obj_t *s_reply;
#endif
extern const lv_font_t kbx_lat_28;

static const char *TAG = "kbx_display";

#define SCENE_TICK_MS       120
#define ERRCARD_MS          12000
#define TALK_TIP_USES       3      /* "Tap to talk" hides after 3 talks (R-1) */
#define MIC_VISUAL_NOISE_FLOOR 4
/* Owner's region until a device setting exists; POSIX TZ for Europe/Berlin. */
#define KBX_DEFAULT_TZ      "CET-1CEST,M3.5.0,M10.5.0/3"
/* Round face: the widest pill content (216 px pill minus its 2 x 16 px
 * padding) that keeps the pill's ends inside the ring (r 133 at the pill's
 * centre line y 254); a dot, an icon or the spinner take their share of it
 * (ui_style.c: dot 9 px, spinner 23 px, symbol about 18 px, each + 8 px gap). */
#define FACE_PILL_TEXT_MAX  184
#define FACE_PILL_DOT_W     17
#define FACE_PILL_ICON_W    26
#define FACE_PILL_SPIN_W    31

static const kbx_display_profile_t *P;
static bool s_watch;                       /* the watch design runs (kbx_design) */
static bool s_face;                        /* round face: everything inside the orb's ring */
static uint32_t s_design_idle_since_ms;
static lv_obj_t *s_chrome, *s_time, *s_wifi, *s_bat;
static kbx_pill_t s_top;
static lv_obj_t *s_caption;
static lv_obj_t *s_hint;
static kbx_pill_t s_pill;
static lv_obj_t *s_err_line, *s_err_btn, *s_err_btn_lbl;

static int s_talk_uses;
static bool s_was_talking;

/* health (ui? / stall log) */
static volatile uint32_t s_ui_timer_ms, s_ui_stalls, s_ui_max_dt_ms, s_ui_last_dt_ms,
                         s_ui_last_stall_log_ms;
static const char *s_chrome_name = "boot";
static esp_timer_handle_t s_ui_health;

/* ------------------------------------------------------------ helpers */

static uint32_t mic_visual_level(uint32_t mic)
{
    if (mic <= MIC_VISUAL_NOISE_FLOOR) return 0;
    mic -= MIC_VISUAL_NOISE_FLOOR;
    return mic > 100 ? 100 : mic;
}

static uint32_t lead_hex(aura_state_t s)
{
    return lv_color_to_u32(aura_orb_lead_color(s)) & 0xFFFFFF;
}

/* Round face: cap the pill text so the whole pill stays inside the ring;
 * longer text ends in "..." (the label's long mode). marks_w = the width the
 * dot, icon and spinner take. Set only on change (style setters redraw). */
static void face_pill_fit(int marks_w)
{
    static int s_text_max = -1;
    if (!s_face) return;
    int m = FACE_PILL_TEXT_MAX - marks_w;
    if (m == s_text_max) return;
    lv_obj_set_style_max_width(s_pill.text, m, 0);
    s_text_max = m;
}

/* Draw nothing for a character the face cannot show (LVGL would draw an
 * empty box): spaces of odd widths become a plain space, zero-width marks and
 * glyphs missing from the face and its fallback are dropped. In place. */
static void caption_sanitize(char *text, const lv_font_t *font)
{
    char *w = text;
    const char *r = text;
    while (*r) {
        const unsigned char c = (unsigned char)*r;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        uint32_t cp = c;
        if (len == 1 && c >= 0x80) cp = 0xFFFD;   /* stray continuation or invalid lead byte */
        if (len > 1) {
            cp = c & (0xFF >> (len + 1));
            for (int i = 1; i < len; i++) {
                if (((unsigned char)r[i] & 0xC0) != 0x80) { len = i; cp = 0xFFFD; break; }
                cp = (cp << 6) | ((unsigned char)r[i] & 0x3F);
            }
        }
        if (cp == 0xA0 || cp == 0x202F || cp == 0x2009 || cp == 0x200A || cp == 0x2002 || cp == 0x2003) {
            *w++ = ' ';
        } else if (cp == '\n' || cp == '\t') {
            *w++ = ' ';
        } else if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x200E || cp == 0x200F ||
                   cp == 0xFEFF || cp == 0xFFFD) {
            /* dropped */
        } else {
            lv_font_glyph_dsc_t dsc;
            if (cp < 0x20 || !lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
                /* dropped: no box for a glyph the display lacks */
            } else {
                memmove(w, r, (size_t)len);
                w += len;
            }
        }
        r += len;
    }
    *w = '\0';
    /* a dropped glyph between two spaces must not leave a double space */
    char *o = text;
    for (const char *i = text; *i; i++) {
        if (*i == ' ' && o > text && o[-1] == ' ') continue;
        *o++ = *i;
    }
    *o = '\0';
}

/* Fit text into `lines` lines of the caption zone. What does not fit is cut at
 * a whole word and closed with an ellipsis, so the last visible line is never
 * a half word. The buffer holds at least 4 spare bytes after the text. */
static void caption_fit_h(char *text, const lv_font_t *font, int width, int avail_h, int max_lines)
{
    int line_h = lv_font_get_line_height(font);
    int lines = (avail_h + 4) / (line_h + 4);
    if (lines < 1) lines = 1;
    if (lines > max_lines) lines = max_lines;
    int max_h = lines * line_h + (lines - 1) * 4;
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, 0, 4, width, LV_TEXT_FLAG_NONE);
    if (sz.y <= max_h) return;
    /* the longest prefix that fits together with the ellipsis (binary search
     * on the byte position, always on a UTF-8 boundary) */
    size_t lo = 0, hi = strlen(text);
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        while (mid > lo && ((unsigned char)text[mid] & 0xC0) == 0x80) mid--;
        if (mid <= lo) break;
        char sv[4];
        memcpy(sv, text + mid, 4);
        memcpy(text + mid, "\xE2\x80\xA6", 4);   /* ellipsis + NUL */
        lv_text_get_size(&sz, text, font, 0, 4, width, LV_TEXT_FLAG_NONE);
        memcpy(text + mid, sv, 4);
        if (sz.y <= max_h) lo = mid; else hi = mid;
    }
    size_t cut = lo;
    /* back to the last whole word when there is a space to cut at */
    size_t k = cut;
    while (k > 0 && text[k] != ' ') k--;
    if (k > 0) cut = k;
    while (cut > 0 && (text[cut - 1] == ' ' || text[cut - 1] == ',' || text[cut - 1] == ';' ||
                       text[cut - 1] == ':')) cut--;
    memcpy(text + cut, "\xE2\x80\xA6", 4);
}

static void caption_fit(char *text, const lv_font_t *font, int width, int avail_h)
{
    caption_fit_h(text, font, width, avail_h, CAPTION_MAX_LINES);
}

/* Length of the sentence terminator at p (ASCII, ellipsis, ideographic and
 * Arabic marks), 0 when p is not one. */
static int sentence_term_len(const char *p)
{
    unsigned char c = (unsigned char)p[0];
    if (c == '.' || c == '!' || c == '?') return 1;
    if (c == 0xD8 && (unsigned char)p[1] == 0x9F) return 2;                       /* U+061F */
    if (c == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0xA6) return 3; /* U+2026 */
    if (c == 0xE3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x82) return 3; /* U+3002 */
    if (c == 0xEF && (unsigned char)p[1] == 0xBC &&
        ((unsigned char)p[2] == 0x9F || (unsigned char)p[2] == 0x81)) return 3;   /* U+FF1F, U+FF01 */
    return 0;
}

/* Keep only the last sentence of an accumulated transcript: the reply
 * overlay shows the current sentence, never the log. A sentence ends at a
 * terminator followed by a space or newline (or a CJK terminator). */
static void last_sentence(char *text)
{
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\n')) text[--n] = '\0';
    const char *start = text;
    for (const char *p = text; *p; ) {
        int t = sentence_term_len(p);
        if (!t) { p++; continue; }
        const char *q = p + t;
        while (sentence_term_len(q)) q += sentence_term_len(q);      /* "?!", "..." */
        if (*q == '\0') break;                                       /* trailing terminator */
        if (*q == ' ' || *q == '\n' || (unsigned char)*p >= 0xE3) {
            while (*q == ' ' || *q == '\n') q++;
            if (*q) start = q;
        }
        p = q;
    }
    if (start != text) memmove(text, start, strlen(start) + 1);
}

static void clock_init(void)
{
    if (!getenv("TZ")) setenv("TZ", KBX_DEFAULT_TZ, 1);
    tzset();
}

/* "HH:MM" once SNTP set the clock, else "". */
static void clock_text(char *out, size_t cap)
{
    time_t now = time(NULL);
    if (now < 1700000000) { out[0] = '\0'; return; }
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* Largest face that shows the whole sentence in the reply overlay (long
 * answers at 28 px kept rewrapping a few words): the 28 px
 * Latin face (ui_theme.h KBX_REPLY_FONT_PX), then the 20 and 16 px faces of
 * the locale; the last one shows the newest lines. */
static const lv_font_t *caption_font_for(const char *text, int width, int avail_h)
{
    const i18n_locale_t loc = i18n_locale();
    const bool latin = loc == I18N_LOCALE_EN || loc == I18N_LOCALE_DE || loc == I18N_LOCALE_ES ||
                       loc == I18N_LOCALE_HI;
    const lv_font_t *faces[3];
    int n = 0;
    if (latin) faces[n++] = &kbx_lat_28;
    faces[n++] = i18n_font(I18N_FONT_20);
    faces[n++] = i18n_font(I18N_FONT_16);
    for (int i = 0; i < n - 1; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, text, faces[i], 0, 4, width, LV_TEXT_FLAG_NONE);
        if (sz.y <= avail_h) return faces[i];
    }
    return faces[n - 1];
}

#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
/* A chat bubble sized to its text (up to max_w); label is moved in, or a
 * new one is created when NULL. */
static lv_obj_t *bubble_create(lv_obj_t *scr, lv_obj_t *label, int max_w)
{
    lv_obj_t *bub = lv_obj_create(scr);
    lv_obj_remove_style_all(bub);
    lv_obj_set_size(bub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bub, BUB_RADIUS, 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bub, BUB_PAD_H, 0);
    lv_obj_set_style_pad_ver(bub, BUB_PAD_V, 0);
    lv_obj_remove_flag(bub, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (!label) {
        label = lv_label_create(bub);
        lv_obj_remove_style_all(label);
    } else {
        lv_obj_set_parent(label, bub);
    }
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, max_w - 2 * BUB_PAD_H, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(label, 4, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);   /* drop the scene's TOP_MID */
    lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
    return bub;
}

/* The user's words: solid ink, paper text, right. The agent: outlined,
 * ink text, left. y is the top (previous) or the bottom edge (current). */
static void bubble_style(lv_obj_t *bub, lv_obj_t *label, int kind, int x, int y, bool bottom)
{
    const bool user = kind == NET_WS_CAPTION_INPUT;
    lv_obj_set_style_bg_color(bub, lv_color_hex(user ? KBX_COL_TEXT_HI : KBX_COL_BG), 0);
    lv_obj_set_style_border_color(bub, lv_color_hex(KBX_COL_TEXT_HI), 0);
    lv_obj_set_style_border_width(bub, user ? 0 : 2, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(user ? KBX_COL_BG : KBX_COL_TEXT_HI), 0);
    if (s_watch) {
        /* centred on the stage, in front of the dimmed orb */
        lv_obj_align(bub, LV_ALIGN_CENTER, 0, (int)P->stage_cy - (int)P->h / 2);
        return;
    }
    if (bottom) {
        lv_obj_align(bub, user ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT, user ? -x : x, y - 300);
    } else {
        lv_obj_align(bub, user ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, user ? -16 : x, y);
    }
}
#endif

/* ---------------------------------------------------------------- tick */

typedef struct {
    aura_state_t aura;
    uint8_t aura_level;
    const char *chrome;
    i18n_id_t pill_id;       /* I18N__COUNT = no pill */
    bool pill_dot;
    bool spinning;
    net_ws_caption_kind_t caption;   /* which transcript may show */
} scene_t;

static i18n_id_t error_message(const kbx_voice_error_t *e)
{
    /* The socket really closed mid-turn: a dropped line, not a fault. */
    if (!strcmp(e->code, "ws_closed")) return I18N_ERR_DROPPED;
    /* An unanswered budget authority or a plan without minutes is an outage
     * of the minutes, never a used-up limit. */
    if (e->kind == KBX_VOICE_ERR_SERVICE && kbx_voice_error_any(e->code, e->reason, "budget_unavailable"))
        return I18N_ERR_BUDGET;
    switch (e->kind) {
    case KBX_VOICE_ERR_OFFLINE: return I18N_ERR_OFFLINE;
    case KBX_VOICE_ERR_SIGNIN: return I18N_ERR_SIGNIN;
    case KBX_VOICE_ERR_SERVICE: return I18N_ERR_SERVICE;
    case KBX_VOICE_ERR_LIMIT: return I18N_ERR_LIMIT;
    case KBX_VOICE_ERR_BUSY: return I18N_ERR_BUSY;
    case KBX_VOICE_ERR_RIGHT: return I18N_ERR_RIGHT;
    case KBX_VOICE_ERR_CONSENT: return I18N_ERR_CONSENT;
    case KBX_VOICE_ERR_EXPIRED: return I18N_ERR_EXPIRED;
    case KBX_VOICE_ERR_REPLY: return I18N_ERR_REPLY;
    default: return I18N_ERR_UNKNOWN;
    }
}

/* R-11 error line + retry; relabels only when the error or locale changed.
 * Returns true while shown. */
static bool error_tick(bool allowed, bool *retry)
{
    static char s_shown[KBX_VOICE_ERROR_CODE_MAX + 24];
    static uint32_t s_shown_epoch = UINT32_MAX;
    kbx_voice_error_t e;
    uint32_t age = 0;
    bool show = allowed && net_ws_last_error(&e, &age) && age < ERRCARD_MS;
    if (show) {
        char key[sizeof(s_shown)];
        snprintf(key, sizeof(key), "%d|%d|%s", (int)e.kind, e.http_status, e.code);
        if (strcmp(key, s_shown) || s_shown_epoch != i18n_epoch()) {
            strlcpy(s_shown, key, sizeof(s_shown));
            s_shown_epoch = i18n_epoch();
            /* One sentence of the Box's own per error kind, in the device language; the
             * server's text and the code go to the log only (net_ws.c, below). It is set
             * in the 20 px face when it fits the free zone, else in 16 px from higher up;
             * what still does not fit ends on a whole word with an ellipsis. */
            static char msg[200];
            strlcpy(msg, i18n_str(error_message(&e)), sizeof(msg) - 4);
            caption_sanitize(msg, i18n_font(I18N_FONT_20));
            const int err_floor = P->pill_cy - 16 - 6;
            const int top20 = P->caption_y - 6;
            lv_point_t sz;
            lv_text_get_size(&sz, msg, i18n_font(I18N_FONT_20), 0, 2, P->caption_w + 12, LV_TEXT_FLAG_NONE);
            const bool small = sz.y > err_floor - top20;
            const i18n_font_size_t fs = small ? I18N_FONT_16 : I18N_FONT_20;
            const int err_top = small ? top20 - 28 : top20;
            i18n_style_label(s_err_line, fs);
            lv_obj_align(s_err_line, LV_ALIGN_TOP_MID, 0, err_top);
            i18n_style_label(s_err_btn_lbl, I18N_FONT_16);
            caption_fit_h(msg, i18n_font(fs), P->caption_w + 12, err_floor - err_top, 8);
            lv_label_set_text(s_err_line, msg);
            lv_label_set_text(s_err_btn_lbl, i18n_str(I18N_UI2_RETRY));
            char code[KBX_VOICE_ERROR_SHORT_MAX];
            kbx_voice_error_short(&e, code, sizeof(code));
            ESP_LOGI(TAG, "error screen: code=%s kind=%d http=%d", code, (int)e.kind, e.http_status);
        }
    }
    /* Sign-in, a plan right and consent are fixed elsewhere, not by tapping
     * again; a limit, an outage or an expired session take a new tap. */
    *retry = show && e.kind != KBX_VOICE_ERR_SIGNIN && e.kind != KBX_VOICE_ERR_RIGHT &&
        e.kind != KBX_VOICE_ERR_CONSENT;
    kbx_set_hidden(s_err_line, !show);
    return show;
}

static void note_chrome(const char *name, uint32_t vu)
{
    if (name == s_chrome_name) return;
    ESP_LOGI(TAG, "chrome %s -> %s vu=%u", s_chrome_name, name, (unsigned)vu);
    s_chrome_name = name;
}

static void scene_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_ui_timer_ms != 0) {
        s_ui_last_dt_ms = now_ms - s_ui_timer_ms;
        if (s_ui_last_dt_ms > s_ui_max_dt_ms) s_ui_max_dt_ms = s_ui_last_dt_ms;
    }
    s_ui_timer_ms = now_ms;

    /* An opaque sheet covers the scene: nothing here needs to move. */
    if (ui_settings_is_open() || ui_panel_progress() > 0) {
        aura_orb_set_paused(true);
        return;
    }
    /* Hold level-driven orb redraws while a handshake runs (PSRAM/cache
     * contention made TLS several times slower). */
    aura_orb_set_paused(net_ws_tls_busy());

    static uint32_t s_epoch = UINT32_MAX;
    bool relocale = s_epoch != i18n_epoch();
    if (relocale) {
        s_epoch = i18n_epoch();
        i18n_style_label(s_caption, I18N_FONT_18);
#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
        {
            const i18n_locale_t loc = i18n_locale();
            if (loc == I18N_LOCALE_EN || loc == I18N_LOCALE_DE || loc == I18N_LOCALE_ES ||
                loc == I18N_LOCALE_HI) {
                lv_obj_set_style_text_font(s_caption, &kbx_lat_28, 0);
            } else {
                i18n_style_label(s_caption, I18N_FONT_20);
            }
        }
#else
        i18n_style_label(s_reply, I18N_FONT_20);
        lv_obj_set_style_base_dir(s_reply, LV_BASE_DIR_AUTO, 0);
#endif
        /* transcripts follow their own script direction, not the locale's */
        lv_obj_set_style_base_dir(s_caption, LV_BASE_DIR_AUTO, 0);
        i18n_style_label(s_hint, I18N_FONT_16);
        i18n_style_label(s_pill.text, I18N_FONT_16);
        i18n_style_label(s_top.text, I18N_FONT_16);
    }

    ui_transient_t tr;
    ui_overlays_transient(&tr);
    uint32_t mic = audio_mic_level();
    uint32_t vmic = mic_visual_level(mic);
    uint32_t spk = audio_speaker_level();
    uint32_t host_age = 0;
    kbx_host_state_t host = box_status_get(&host_age);
    bool host_active = host != KBX_HOST_IDLE && host_age < 30000;
    if (host == KBX_HOST_DONE && host_age > 2500) host_active = false;
    const bool connecting = net_ws_talk_connecting();
    const bool conversation = net_ws_conversation_active();
    const bool local_listen = net_ws_talk_active() && !connecting && net_ws_is_connected();
    const bool muted = audio_local_mute();

    /* talk tip counter: a talk that actually started counts as one use */
    bool talking = net_ws_talk_active();
    if (talking && !s_was_talking && s_talk_uses < TALK_TIP_USES) s_talk_uses++;
    s_was_talking = talking;

    /* A design chosen in the settings takes effect through a clean restart
     * once the box has been out of a conversation for a few seconds (the
     * applied settings revision is stored by then). */
    if (kbx_design_restart_needed()) {
        const bool busy = net_ws_talk_active() || connecting || host_active || audio_is_speaking();
        if (busy) s_design_idle_since_ms = 0;
        else if (!s_design_idle_since_ms) s_design_idle_since_ms = now_ms | 1U;
        else if (now_ms - s_design_idle_since_ms > 4000) {
            ESP_LOGW(TAG, "display design %s: restarting", kbx_design_id(kbx_design_stored()));
            esp_restart();
        }
    }

    scene_t sc = {
        .aura = AURA_INACTIVE, .aura_level = (uint8_t)vmic, .chrome = "idle",
        .pill_id = I18N__COUNT, .pill_dot = true, .caption = NET_WS_CAPTION_NONE,
    };
    if (muted) {
        sc.aura_level = 0;
        sc.chrome = "mute";
    } else if (connecting) {
        sc.aura = AURA_CONNECTING;
        sc.chrome = "host";
        sc.pill_id = I18N_STATUS_CONNECTING;
    } else if (host_active) {
        sc.chrome = "host";
        switch (host) {
        case KBX_HOST_WAKE:
            sc.aura = AURA_CONNECTING; sc.pill_id = I18N_STATE_WAKE; break;
        case KBX_HOST_LISTENING:
            sc.aura = AURA_LISTENING; sc.pill_id = I18N_STATE_LISTENING;
            sc.caption = NET_WS_CAPTION_INPUT; break;
        case KBX_HOST_THINKING:
            sc.aura = AURA_PROCESSING; sc.pill_id = I18N_STATE_THINKING; sc.spinning = true;
            sc.caption = NET_WS_CAPTION_INPUT; break;
        case KBX_HOST_SPEAKING:
            sc.aura = AURA_SPEAKING; sc.aura_level = (uint8_t)spk; sc.pill_id = I18N_STATE_ANSWERING;
            sc.caption = NET_WS_CAPTION_OUTPUT; break;
        case KBX_HOST_DONE:
            sc.aura = AURA_SETTLING; sc.pill_id = I18N_STATE_DONE;
            sc.caption = NET_WS_CAPTION_OUTPUT; break;
        case KBX_HOST_ERROR:
            sc.aura = AURA_ERROR; sc.pill_id = I18N_STATE_ERROR; break;
        default: break;
        }
    } else if (audio_is_speaking()) {
        sc.aura = AURA_SPEAKING; sc.aura_level = (uint8_t)spk; sc.chrome = "speak";
        sc.pill_id = I18N_STATE_ANSWERING;
        sc.caption = NET_WS_CAPTION_OUTPUT;
    } else if (local_listen) {
        sc.aura = AURA_LISTENING; sc.chrome = "listen"; sc.pill_id = I18N_STATE_LISTENING;
        sc.caption = NET_WS_CAPTION_INPUT;
    } else {
        /* idle: only say something when the box cannot talk yet */
        sc.aura_level = 0;
        if (!audio_speaker_ready()) sc.pill_id = I18N_STATE_SPEAKER_CHECK;
        else if (!net_ws_ready_for_talk()) sc.pill_id = I18N_STATUS_CONNECTING;
        /* "Tap to talk" only where there is something to tap */
        else if (UI_HAS_TOUCH && s_talk_uses < TALK_TIP_USES) { sc.pill_id = I18N_UI2_TAP_TO_TALK; sc.pill_dot = false; }
    }
    if (sc.pill_id == I18N_STATUS_CONNECTING) sc.spinning = false;
    /* The Companion holds this turn (spoken Companion request): its answer takes
     * longer than the quick one, so say whom the box is waiting for. */
    if (!muted && net_ws_route_switch_active() &&
        (sc.pill_id == I18N_STATE_THINKING || sc.pill_id == I18N_STATUS_CONNECTING)) {
        sc.aura = AURA_PROCESSING;
        sc.pill_id = I18N_STATE_COMPANION;
        sc.spinning = true;
    }
    /* Voice Agent tool running: say so instead of a silent "thinking". */
    if (!muted && sc.aura != AURA_INACTIVE && net_ws_tool_activity(NULL, 0)) {
        sc.aura = AURA_PROCESSING;
        sc.pill_id = I18N_STATE_TOOL;
        sc.spinning = true;
    }

    /* R-11: a failed tap rests the orb in the error palette. */
    bool retry = false;
    bool err = error_tick(!muted && !tr.info, &retry);
    if (err) {
        sc.aura = AURA_ERROR;
        sc.aura_level = 0;
        sc.caption = NET_WS_CAPTION_NONE;
    }

    aura_orb_set_dim(muted);
    aura_orb_set_state(sc.aura);
    aura_orb_set_level(sc.aura_level);
    note_chrome(sc.chrome, vmic);

    /* top chrome: mute chip > conversation chip > idle clock */
    bool idle = sc.aura == AURA_INACTIVE && !muted;
    if (s_watch && P->shape == KBX_SHAPE_WIDE) {
        /* RLCD watch: the rim carries the mute symbol and the conversation word */
        kbx_set_hidden(s_top.obj, true);
    } else if (muted) {
        kbx_pill_set_kind(&s_top, KBX_PILL_ALERT);
        kbx_pill_set(&s_top, 0, LV_SYMBOL_MUTE, i18n_str(I18N_UI2_MIC_OFF));
        kbx_set_hidden(s_top.obj, false);
    } else if (conversation) {
        kbx_pill_set_kind(&s_top, KBX_PILL_BRAND);
        kbx_pill_set(&s_top, 0, NULL, i18n_str(I18N_CONVERSATION));
        kbx_set_hidden(s_top.obj, false);
    } else {
        kbx_set_hidden(s_top.obj, true);
    }
    char hhmm[8];
    clock_text(hhmm, sizeof(hhmm));
    bool wifi = net_wifi_is_connected(NULL, 0);
    kbx_set_text(s_time, hhmm);
    kbx_set_hidden(s_time, hhmm[0] == '\0');
    static int s_wifi_drawn = -1;
    if (s_wifi_drawn != (int)wifi) {
        /* style setters invalidate even when the value is unchanged */
        lv_obj_set_style_text_color(s_wifi, lv_color_hex(wifi ? KBX_COL_TEXT_MID : KBX_COL_TEXT_LO), 0);
        lv_obj_set_style_opa(s_wifi, wifi ? LV_OPA_COVER : LV_OPA_40, 0);
        s_wifi_drawn = wifi;
    }
    if (s_watch && P->shape != KBX_SHAPE_WIDE) {
        /* round watch face: the battery joins the time in the top rim,
         * where the board has a gauge */
        const int pct = battery_percent();
        char bat[24] = "";
        if (pct >= 0) {
            const char *sym = pct >= 85 ? LV_SYMBOL_BATTERY_FULL : pct >= 60 ? LV_SYMBOL_BATTERY_3
                            : pct >= 35 ? LV_SYMBOL_BATTERY_2 : pct >= 12 ? LV_SYMBOL_BATTERY_1
                            : LV_SYMBOL_BATTERY_EMPTY;
            snprintf(bat, sizeof(bat), "%s %d%%", sym, pct);
        }
        kbx_set_text(s_bat, bat);
        kbx_set_hidden(s_bat, bat[0] == '\0');
    }
    kbx_set_hidden(s_chrome, (s_watch && P->shape == KBX_SHAPE_WIDE) ||
                             !(idle && !err && lv_obj_has_flag(s_top.obj, LV_OBJ_FLAG_HIDDEN)));

    /* caption: info line > the one current sentence of the live transcript
     * (reply overlay, with the orb dimmed behind it) */
    static EXT_RAM_BSS_ATTR char s_cap[CAP_BUF];
    static uint32_t s_cap_seq = UINT32_MAX;
    static int s_cap_info = -1;
    bool cap_shown = false;
    bool reply_shown = false;
    if (tr.info) {
        if (s_cap_info != (int)tr.info_id || relocale) {
#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
            if (s_watch) {
                if (s_cap_kind != NET_WS_CAPTION_OUTPUT) {
                    bubble_style(s_cur_bub, s_caption, NET_WS_CAPTION_OUTPUT, 16, CUR_BOTTOM, true);
                }
                s_cap_kind = NET_WS_CAPTION_OUTPUT;
            }
#endif
            lv_obj_set_style_text_color(s_caption, lv_color_hex(KBX_COL_TEXT_HI), 0);
            lv_label_set_text(s_caption, i18n_str(tr.info_id));
            s_cap_info = (int)tr.info_id;
            s_cap_seq = UINT32_MAX;
        }
        cap_shown = true;
    } else if (sc.caption != NET_WS_CAPTION_NONE) {
        static EXT_RAM_BSS_ATTR char buf[CAP_BUF];   /* static PSRAM: small LVGL stack, short internal RAM */
        net_ws_caption_kind_t kind;
        bool final = false;
        uint32_t seq = 0;
        bool has = net_ws_caption(buf, sizeof(buf) - 4, &kind, &final, &seq);
        /* listening/thinking show the user's words, speaking the answer;
         * a speaking turn without output text keeps the final question */
        bool fits = has && (kind == sc.caption ||
                            (sc.caption == NET_WS_CAPTION_OUTPUT && kind == NET_WS_CAPTION_INPUT));
        if (fits) {
            if (seq != s_cap_seq || relocale) {
                /* The voice layer keeps only the tail of a very long reply (net_ws
                 * CAPTION_MAX): when no sentence start survives in it, the text begins
                 * inside a word. Start on the next whole word instead. */
                const bool tail_cut = strlen(buf) >= 560;
                const size_t before_len = strlen(buf);
                last_sentence(buf);
                if (tail_cut && strlen(buf) == before_len) {
                    char *sp = strchr(buf, ' ');
                    if (sp && sp - buf < 32) memmove(buf, sp + 1, strlen(sp + 1) + 1);
                }
#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
                if (kind != s_cap_kind) bubble_style(s_cur_bub, s_caption, kind, 16, CUR_BOTTOM, true);
                s_cap_kind = kind;
                const lv_font_t *f = caption_font_for(buf, CUR_W - 2 * BUB_PAD_H, CUR_H - 2 * BUB_PAD_V);
                if (lv_obj_get_style_text_font(s_caption, 0) != f) lv_obj_set_style_text_font(s_caption, f, 0);
                caption_sanitize(buf, f);
                caption_fit(buf, f, CUR_W - 2 * BUB_PAD_H, CUR_H - 2 * BUB_PAD_V);
                strlcpy(s_cap, buf, sizeof(s_cap));
                lv_label_set_text(s_caption, s_cap);
#else
                const lv_font_t *f = caption_font_for(buf, P->caption_w, REPLY_H);
                if (lv_obj_get_style_text_font(s_reply, 0) != f) lv_obj_set_style_text_font(s_reply, f, 0);
                caption_sanitize(buf, f);
                caption_fit(buf, f, P->caption_w, REPLY_H);
                strlcpy(s_cap, buf, sizeof(s_cap));
                bool partial = kind == NET_WS_CAPTION_INPUT && !final && sc.aura == AURA_LISTENING;
                lv_obj_set_style_text_color(s_reply,
                    lv_color_hex(partial ? KBX_COL_TEXT_MID : KBX_COL_TEXT_HI), 0);
                lv_label_set_text(s_reply, s_cap);
#endif
                s_cap_seq = seq;
                s_cap_info = -1;
            }
            reply_shown = true;
        }
    }
#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
    kbx_set_hidden(s_caption, !(cap_shown || reply_shown) || err);
    kbx_set_hidden(s_cur_bub, !(cap_shown || reply_shown) || err);
#else
    kbx_set_hidden(s_caption, !cap_shown || err);
    kbx_set_hidden(s_reply, !reply_shown || err);
#endif
    /* the watch design's big orb and the round face sit behind the info line
     * and the error text too */
    aura_orb_set_backdrop(s_watch || s_face ? (reply_shown || cap_shown || err) : (reply_shown && !err));

    /* bottom zone: lock hint > volume > mute hint > error > state pill */
    char line[64];
    if (tr.lock) {
        if (tr.lock_remaining > 0) {
            /* Table strings carry exactly one %d (checked by i18n/gen.mjs). */
            snprintf(line, sizeof(line), i18n_str(I18N_LOCK_TAP_LEFT), tr.lock_remaining);
        } else {
            strlcpy(line, i18n_str(I18N_LOCK_UNLOCKED), sizeof(line));
        }
        kbx_pill_set_kind(&s_pill, KBX_PILL_NEUTRAL);
        face_pill_fit(FACE_PILL_ICON_W);
        kbx_pill_set(&s_pill, 0, LV_SYMBOL_EYE_CLOSE, line);
        kbx_pill_set_spinning(&s_pill, false);
        kbx_set_hidden(s_pill.obj, false);
        kbx_set_hidden(s_hint, true);
    } else if (tr.volume) {
        snprintf(line, sizeof(line), "%u %%", (unsigned)audio_volume());
        kbx_pill_set_kind(&s_pill, KBX_PILL_BRAND);
        face_pill_fit(FACE_PILL_ICON_W);
        kbx_pill_set(&s_pill, 0, LV_SYMBOL_VOLUME_MAX, line);
        kbx_pill_set_spinning(&s_pill, false);
        kbx_set_hidden(s_pill.obj, false);
        kbx_set_hidden(s_hint, true);
    } else if (muted) {
        kbx_set_text(s_hint, i18n_str(I18N_UI2_HOLD_UNMUTE));
        kbx_set_hidden(s_hint, false);
        kbx_set_hidden(s_pill.obj, true);
        kbx_pill_set_spinning(&s_pill, false);
    } else if (err) {
        kbx_set_hidden(s_pill.obj, true);
        kbx_set_hidden(s_hint, true);
        kbx_pill_set_spinning(&s_pill, false);
    } else if (sc.pill_id != I18N__COUNT) {
        kbx_pill_set_kind(&s_pill, KBX_PILL_NEUTRAL);
        uint32_t dot = !sc.pill_dot ? 0 :
            (sc.aura == AURA_INACTIVE ? lead_hex(AURA_CONNECTING) : lead_hex(sc.aura));
        face_pill_fit((dot ? FACE_PILL_DOT_W : 0) + (sc.spinning ? FACE_PILL_SPIN_W : 0));
        kbx_pill_set(&s_pill, dot, NULL, i18n_str(sc.pill_id));
        kbx_pill_set_spinning(&s_pill, sc.spinning);
        kbx_set_hidden(s_pill.obj, false);
        kbx_set_hidden(s_hint, true);
    } else {
        kbx_set_hidden(s_pill.obj, true);
        kbx_set_hidden(s_hint, true);
        kbx_pill_set_spinning(&s_pill, false);
    }
    kbx_set_hidden(s_err_btn, !(err && retry && !tr.lock && !tr.volume));
#if CONFIG_KBX_HOME_SCREEN
    {
        const bool quiet = sc.aura == AURA_INACTIVE && !err && !tr.info && !audio_is_speaking() &&
                           audio_play_agent_idle();
        if (s_watch) {
            /* The rims stay; the room climate fills the bottom one while the
             * box is quiet and gives it to the state pill otherwise. */
            ui_home_watch_tick(quiet && sc.pill_id == I18N__COUNT && !tr.lock && !tr.volume && !muted, muted,
                               conversation ? i18n_str(I18N_CONVERSATION) : NULL);
        } else {
            /* Room display while idle; the voice scene owns turns, errors and info.
             * Keep the voice scene while the reply's last audio still plays. */
            ui_home_tick(quiet, muted);
        }
    }
#endif
}

/* ---------------------------------------------------------------- build */

void ui_scene_build(lv_display_t *disp)
{
    P = kbx_display_profile();
    s_watch = kbx_design() == KBX_DESIGN_WATCH;
#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
    if (s_watch) {
        s_cur_h = 196;                       /* between the rims, 4 px air */
        s_max_lines = 5;
    }
#endif
    clock_init();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* The orb's ring is the scene's only ring (round face: the full display). */
    s_face = P->shape == KBX_SHAPE_ROUND;
    if (aura_orb_create(scr, P) != ESP_OK) ESP_LOGE(TAG, "aura orb init failed");
#if CONFIG_KBX_HOME_SCREEN
    /* watch: the rims go over the orb and under every other widget */
    if (s_watch) ui_home_watch_build(scr);
#endif

    /* top chrome (R-1): time + Wi-Fi above the mark (round face) or on the
     * top chord; the round watch design shows the time bold at 28 px */
    const bool bold_time = s_watch && s_face;
    s_chrome = kbx_box(scr);
    lv_obj_set_size(s_chrome, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_chrome, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chrome, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_chrome, 10, 0);
    lv_obj_align(s_chrome, LV_ALIGN_TOP_MID, 0, P->chrome_cy - (bold_time ? 16 : 10));
    s_time = lv_label_create(s_chrome);
    lv_obj_remove_style_all(s_time);
    lv_obj_set_style_text_font(s_time, bold_time ? &lv_font_montserrat_28 : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(bold_time ? KBX_COL_TEXT_HI : KBX_COL_TEXT_MID), 0);
    lv_label_set_text(s_time, "");
    s_wifi = lv_label_create(s_chrome);
    lv_obj_remove_style_all(s_wifi);
    lv_obj_set_style_text_font(s_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    s_bat = lv_label_create(s_chrome);
    lv_obj_remove_style_all(s_bat);
    lv_obj_set_style_text_font(s_bat, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bat, lv_color_hex(KBX_COL_TEXT_MID), 0);
    lv_label_set_text(s_bat, "");
    lv_obj_add_flag(s_bat, LV_OBJ_FLAG_HIDDEN);

    kbx_pill_create(&s_top, scr);
    lv_obj_set_height(s_top.obj, 30);
    lv_obj_set_style_radius(s_top.obj, 15, 0);
    lv_obj_set_style_pad_hor(s_top.obj, 12, 0);   /* stays inside the ring on the top chord */
    lv_obj_align(s_top.obj, LV_ALIGN_TOP_MID, 0, P->chrome_cy - 13);
    lv_obj_add_flag(s_top.obj, LV_OBJ_FLAG_HIDDEN);

    s_caption = kbx_text_block(scr, I18N_FONT_18, KBX_COL_TEXT_HI, P->caption_w);
    lv_obj_align(s_caption, LV_ALIGN_TOP_MID, 0, P->caption_y);
    lv_obj_add_flag(s_caption, LV_OBJ_FLAG_HIDDEN);
#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
    s_cur_bub = bubble_create(scr, s_caption, CUR_W);
    bubble_style(s_cur_bub, s_caption, NET_WS_CAPTION_OUTPUT, 16, CUR_BOTTOM, true);
#else
    /* reply overlay: centred on the stage, in front of the dimmed orb */
    s_reply = kbx_text_block(scr, I18N_FONT_20, KBX_COL_TEXT_HI, P->caption_w);
    lv_obj_align(s_reply, LV_ALIGN_CENTER, 0, (int)P->stage_cy - (int)P->h / 2 - REPLY_RAISE);
    lv_obj_add_flag(s_reply, LV_OBJ_FLAG_HIDDEN);
#endif

    kbx_pill_create(&s_pill, scr);
    lv_obj_align(s_pill.obj, LV_ALIGN_TOP_MID, 0, P->pill_cy - 17);
    lv_obj_add_flag(s_pill.obj, LV_OBJ_FLAG_HIDDEN);

    /* on the RLCD watch design the hint sits in the solid bottom rim: paper on ink */
    s_hint = kbx_text_block(scr, I18N_FONT_16, s_watch && P->shape == KBX_SHAPE_WIDE ? KBX_COL_BG : KBX_COL_TEXT_MID,
                            P->caption_w);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, P->pill_cy - 20);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    /* R-11 error: one line (20 px), a retry pill (the whole screen is the
     * tap target), and the short reason code in secondary text */
    s_err_line = kbx_text_block(scr, I18N_FONT_20, KBX_COL_TEXT_HI, P->caption_w + 12);
    lv_obj_align(s_err_line, LV_ALIGN_TOP_MID, 0, P->caption_y - 6);
    lv_obj_add_flag(s_err_line, LV_OBJ_FLAG_HIDDEN);
    s_err_btn = kbx_button(scr, KBX_BTN_TINT, 196, NULL, &s_err_btn_lbl);
    lv_obj_remove_flag(s_err_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(s_err_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_err_btn, 20, 0);
    lv_obj_set_flex_flow(s_err_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_err_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(s_err_btn_lbl, LV_SIZE_CONTENT);
    /* round face: a 190 px button keeps its corners inside the ring */
    lv_obj_set_style_max_width(s_err_btn_lbl, s_face ? 150 : 180, 0);
    lv_obj_align(s_err_btn, LV_ALIGN_TOP_MID, 0, P->pill_cy - 16);
    lv_obj_add_flag(s_err_btn, LV_OBJ_FLAG_HIDDEN);

#if CONFIG_KBX_HOME_SCREEN
    if (!s_watch) ui_home_build(scr);
#endif
    ui_overlays_build(disp);
    lv_timer_create(scene_timer_cb, SCENE_TICK_MS, NULL);
}

/* ---------------------------------------------------------------- health */

static void ui_health_cb(void *arg)
{
    (void)arg;
    const uint32_t last = s_ui_timer_ms;
    if (last == 0) return;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t dt_ms = now_ms - last;
    if (dt_ms > s_ui_max_dt_ms) s_ui_max_dt_ms = dt_ms;
    if (dt_ms < 500) return;
    s_ui_stalls++;
    if ((now_ms - s_ui_last_stall_log_ms) < 5000) return;
    s_ui_last_stall_log_ms = now_ms;
    ESP_LOGE(TAG, "error_code=ui_timer_stall dt_ms=%u stalls=%u vu=%u chrome=%s paused=%d heap_int=%u",
             (unsigned)dt_ms, (unsigned)s_ui_stalls, (unsigned)audio_mic_level(), s_chrome_name,
             (int)aura_orb_paused(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void ui_scene_health_start(void)
{
    if (s_ui_health) return;
    const esp_timer_create_args_t args = { .callback = ui_health_cb, .name = "kbx_ui_health" };
    if (esp_timer_create(&args, &s_ui_health) != ESP_OK) {
        ESP_LOGW(TAG, "ui health timer missing");
        return;
    }
    (void)esp_timer_start_periodic(s_ui_health, 250000);
}

void ui_scene_log_status(void)
{
    ESP_LOGI(TAG, "status: profile=%s chrome=%s vu=%u dt_ms=%u max_dt_ms=%u stalls=%u paused=%d "
             "aura=%d heap_int=%u",
             P ? P->id : "-", s_chrome_name, (unsigned)audio_mic_level(), (unsigned)s_ui_last_dt_ms,
             (unsigned)s_ui_max_dt_ms, (unsigned)s_ui_stalls, (int)aura_orb_paused(),
             (int)aura_orb_state(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ui_overlays_log_status();
}
