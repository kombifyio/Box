/*
 * kombify box - RLCD 4.2 home screen (wide 400x300, 1 bit, no touch).
 *
 * A reflective mono panel reads like paper, so the idle screen is a room
 * display instead of the orb: clock, date, room temperature and humidity
 * from the onboard SHTC3, and the kombify wordmark (the colour rosette
 * dithers to noise at this size in one bit). The moment a voice turn
 * starts (listening, thinking, speaking), an error or an info line
 * appears, the home layer hides and the full voice scene with the large
 * orb takes over; it returns when the turn ends.
 *
 * Layout on an 8 px grid, above the scene's bottom status zone (y >= 258):
 *   top row    y 8..40     date (left), mute + Wi-Fi + wordmark (right)
 *   clock      y 44..136   solid ink card, HH:MM 96 px SemiBold in paper
 *   climate    y 148..248  two outlined cards (176 x 100, 2 px rule,
 *                          16 px gutter): label 16 px, value 44 px, unit
 *                          20 px on the value's baseline
 * Colours are the dark UI 2 tokens; the RLCD flush inverts them, so
 * KBX_COL_BG is paper and KBX_COL_TEXT_HI is solid ink.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "assets/kombify_rosette.h"
#include "aura_orb.h"
#include "battery.h"
#include "env_sensor.h"
#include "i18n.h"
#include "net_wifi.h"
#include "ui_home.h"
#include "ui_style.h"
#include "ui_theme.h"

#define HOME_H        256
#define M             16                      /* margins and gutters */
#define RADIUS        20
#define CLOCK_Y       44
#define CLOCK_H       92                      /* 70 px line box + 11 px air */
#define CARD_W        ((400 - 3 * M) / 2)     /* 176 */
#define CARD_Y        (CLOCK_Y + CLOCK_H + 12)
#define CARD_H        100
#define CARD_RULE     2
#define MARK_D        26                      /* idle kombify mark, px */

extern const lv_font_t kbx_home_96;
extern const lv_font_t kbx_home_44;

static lv_obj_t *s_home;
static lv_obj_t *s_date, *s_status, *s_word, *s_mark, *s_clock;
static lv_obj_t *s_temp, *s_temp_unit, *s_temp_lbl, *s_rh, *s_rh_unit, *s_rh_lbl;
static uint32_t s_epoch = UINT32_MAX;

static lv_obj_t *home_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *home_card(int x, int y, int w, int h, bool solid)
{
    lv_obj_t *card = lv_obj_create(s_home);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, RADIUS, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(solid ? KBX_COL_TEXT_HI : KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    if (!solid) {
        lv_obj_set_style_border_color(card, lv_color_hex(KBX_COL_TEXT_HI), 0);
        lv_obj_set_style_border_width(card, CARD_RULE, 0);
    }
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return card;
}

/* Outlined card: label top-left, large value, small unit on its baseline. */
static void climate_card(int x, lv_obj_t **value, lv_obj_t **unit, lv_obj_t **label)
{
    lv_obj_t *card = home_card(x, CARD_Y, CARD_W, CARD_H, false);
    *label = kbx_label(card, I18N_FONT_16, KBX_COL_TEXT_HI);
    lv_obj_set_pos(*label, M - CARD_RULE, 12 - CARD_RULE);
    *value = home_label(card, &kbx_home_44, KBX_COL_TEXT_HI);
    lv_obj_set_pos(*value, M - CARD_RULE, 46 - CARD_RULE);
    *unit = home_label(card, &lv_font_montserrat_20, KBX_COL_TEXT_HI);
}

/* The unit sits on the value's baseline (kbx_home_44 keeps 7 px below it,
 * Montserrat 20 about 4). */
static void place_unit(lv_obj_t *value, lv_obj_t *unit)
{
    lv_obj_align_to(unit, value, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -3);
}

void ui_home_build(lv_obj_t *scr)
{
    s_home = lv_obj_create(scr);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, LV_PCT(100), HOME_H);
    lv_obj_set_pos(s_home, 0, 0);
    lv_obj_set_style_bg_color(s_home, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* top row, centre line y = 24, text inset to the cards' content edge */
    s_date = kbx_label(s_home, I18N_FONT_18, KBX_COL_TEXT_HI);
    lv_obj_set_pos(s_date, 2 * M - CARD_RULE, 13);

    s_word = home_label(s_home, &lv_font_montserrat_18, KBX_COL_TEXT_HI);
    lv_label_set_text(s_word, "kombify");
    lv_obj_set_style_text_letter_space(s_word, 1, 0);
    lv_obj_align(s_word, LV_ALIGN_TOP_RIGHT, -(2 * M - CARD_RULE + MARK_D + 6), 13);

    /* The kombify mark rests greyed and dimmed (dithered on this panel) while
     * the box is idle; the voice scene shows it solid once a turn starts. */
    s_mark = aura_orb_idle_mark_create(s_home, MARK_D);
    lv_obj_align(s_mark, LV_ALIGN_TOP_RIGHT, -(2 * M - CARD_RULE), 10);

    s_status = home_label(s_home, &lv_font_montserrat_18, KBX_COL_TEXT_HI);

    lv_obj_t *clock_card = home_card(M, CLOCK_Y, 400 - 2 * M, CLOCK_H, true);
    s_clock = home_label(clock_card, &kbx_home_96, KBX_COL_BG);
    lv_obj_set_width(s_clock, LV_PCT(100));
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, 0);

    climate_card(M, &s_temp, &s_temp_unit, &s_temp_lbl);
    climate_card(2 * M + CARD_W, &s_rh, &s_rh_unit, &s_rh_lbl);
    lv_label_set_text(s_temp_unit, "\xC2\xB0" "C");
    lv_label_set_text(s_rh_unit, "%");
}

static bool comma_decimal(void)
{
    i18n_locale_t l = i18n_locale();
    return l == I18N_LOCALE_DE || l == I18N_LOCALE_ES;
}

void ui_home_tick(bool show, bool muted)
{
    if (!s_home) return;
    kbx_set_hidden(s_home, !show);
    aura_orb_set_hidden(show);
    if (!show) return;

    if (s_epoch != i18n_epoch()) {
        s_epoch = i18n_epoch();
        i18n_style_label(s_date, I18N_FONT_18);
        i18n_style_label(s_temp_lbl, I18N_FONT_16);
        i18n_style_label(s_rh_lbl, I18N_FONT_16);
        lv_label_set_text(s_temp_lbl, i18n_str(I18N_HOME_TEMP));
        lv_label_set_text(s_rh_lbl, i18n_str(I18N_HOME_HUMIDITY));
    }

    char buf[48];
    time_t now = time(NULL);
    if (now >= 1700000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        kbx_set_text(s_clock, buf);
        switch (i18n_locale()) {
        case I18N_LOCALE_DE: strftime(buf, sizeof(buf), "%d.%m.%Y", &tm); break;
        case I18N_LOCALE_ES: strftime(buf, sizeof(buf), "%d/%m/%Y", &tm); break;
        default: strftime(buf, sizeof(buf), "%Y-%m-%d", &tm); break;
        }
        kbx_set_text(s_date, buf);
    } else {
        kbx_set_text(s_clock, "");
        kbx_set_text(s_date, "");
    }

    float t, rh;
    if (env_sensor_get(&t, &rh)) {
        int tenths = (int)lroundf(t * 10.0f);
        snprintf(buf, sizeof(buf), "%s%d%c%d", tenths < 0 ? "-" : "", abs(tenths) / 10,
                 comma_decimal() ? ',' : '.', abs(tenths) % 10);
        kbx_set_text(s_temp, buf);
        snprintf(buf, sizeof(buf), "%d", (int)lroundf(rh));
        kbx_set_text(s_rh, buf);
    } else {
        kbx_set_text(s_temp, "-");
        kbx_set_text(s_rh, "-");
    }
    place_unit(s_temp, s_temp_unit);
    place_unit(s_rh, s_rh_unit);

    const bool wifi = net_wifi_is_connected(NULL, 0);
    /* battery level: symbol + percent, left of Wi-Fi */
    char bat[24] = "";
    const int pct = battery_percent();
    if (pct >= 0) {
        const char *sym = pct >= 85 ? LV_SYMBOL_BATTERY_FULL : pct >= 60 ? LV_SYMBOL_BATTERY_3
                        : pct >= 35 ? LV_SYMBOL_BATTERY_2 : pct >= 12 ? LV_SYMBOL_BATTERY_1
                        : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(bat, sizeof(bat), "%d%% %s  ", pct, sym);
    }
    snprintf(buf, sizeof(buf), "%s%s%s%s", muted ? LV_SYMBOL_MUTE : "", muted ? "  " : "", bat,
             wifi ? LV_SYMBOL_WIFI : "");
    kbx_set_text(s_status, buf);
    lv_obj_align_to(s_status, s_word, LV_ALIGN_OUT_LEFT_MID, -10, 0);
}
