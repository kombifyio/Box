/*
 * kombify box - RLCD 4.2 rims of the watch design (wide 400x300, 1 bit, no
 * touch; display_design.h "watch").
 *
 * The watch design wears the layout of the round boards (the Pixel Watch
 * design language): the Aura Orb is the face, here a big greyed mark on the
 * paper while the box is idle, and the secondary information sits in two
 * solid ink rims, like the bezel of a watch:
 *
 *   top rim     y 0..48    date (or the conversation word) left, the time
 *                          in the 44 px digit face in the middle, mute,
 *                          battery and Wi-Fi right
 *   bottom rim  y 252..300 room temperature and humidity from the onboard
 *                          SHTC3 while the box is idle; a voice state, lock
 *                          or volume pill (ui_scene.c) takes the rim over
 *
 * The rims keep their 24 px inner corners and stay on screen through every
 * voice turn, so the time never goes away. Colours are the dark UI 2 tokens;
 * the RLCD flush inverts them, so KBX_COL_BG is paper and KBX_COL_TEXT_HI is
 * solid ink: the rims are filled with ink and carry paper text.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "battery.h"
#include "env_sensor.h"
#include "i18n.h"
#include "net_wifi.h"
#include "ui_home.h"
#include "ui_style.h"
#include "ui_theme.h"

#define SCREEN_W    400
#define SCREEN_H    300
#define RIM_H       48
#define RIM_R       24                      /* inner corner radius */
#define M           16                      /* side margin */
#define DIGIT_H     31                      /* cap height of the 44 px digit face */

extern const lv_font_t kbx_home_44;

static lv_obj_t *s_date, *s_status, *s_hh, *s_mm, *s_wifi;
static lv_obj_t *s_climate;                 /* bottom rim content, hidden while a pill is shown */
static lv_obj_t *s_temp, *s_temp_unit, *s_rh, *s_rh_unit;
static uint32_t s_epoch = UINT32_MAX;

static lv_obj_t *rim_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(KBX_COL_BG), 0);
    lv_label_set_text(l, "");
    return l;
}

/* One solid ink rim: `top` rounds the lower corners, otherwise the upper
 * ones; the rest of the box sits off screen. */
static void rim_create(lv_obj_t *scr, bool top)
{
    lv_obj_t *rim = lv_obj_create(scr);
    lv_obj_remove_style_all(rim);
    lv_obj_set_size(rim, SCREEN_W, RIM_H + RIM_R);
    lv_obj_set_pos(rim, 0, top ? -RIM_R : SCREEN_H - RIM_H);
    lv_obj_set_style_radius(rim, RIM_R, 0);
    lv_obj_set_style_bg_color(rim, lv_color_hex(KBX_COL_TEXT_HI), 0);
    lv_obj_set_style_bg_opa(rim, LV_OPA_COVER, 0);
    lv_obj_remove_flag(rim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/* A value in the digit face with its unit on the value's baseline. */
static lv_obj_t *climate_group(lv_obj_t *parent, lv_obj_t **value, lv_obj_t **unit)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(g, 4, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    *value = rim_label(g, &kbx_home_44);
    *unit = rim_label(g, &lv_font_montserrat_20);
    return g;
}

void ui_home_watch_build(lv_obj_t *scr)
{
    rim_create(scr, true);
    rim_create(scr, false);

    /* top rim */
    s_date = kbx_label(scr, I18N_FONT_18, KBX_COL_BG);
    lv_obj_set_pos(s_date, M, 13);

    /* the time: two digit pairs and a colon of two squares (the 44 px face
     * has no colon glyph) */
    lv_obj_t *clock = lv_obj_create(scr);
    lv_obj_remove_style_all(clock);
    lv_obj_set_size(clock, LV_SIZE_CONTENT, DIGIT_H + 8);
    lv_obj_set_flex_flow(clock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock, 6, 0);
    lv_obj_remove_flag(clock, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(clock, LV_ALIGN_TOP_MID, 0, 4);
    s_hh = rim_label(clock, &kbx_home_44);
    lv_obj_t *colon = lv_obj_create(clock);
    lv_obj_remove_style_all(colon);
    lv_obj_set_size(colon, 6, DIGIT_H);
    lv_obj_remove_flag(colon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *dot = lv_obj_create(colon);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_pos(dot, 0, i ? 20 : 5);
        lv_obj_set_style_bg_color(dot, lv_color_hex(KBX_COL_BG), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
    s_mm = rim_label(clock, &kbx_home_44);

    s_status = rim_label(scr, &lv_font_montserrat_18);
    s_wifi = rim_label(scr, &lv_font_montserrat_18);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_RIGHT, -M, 13);

    /* bottom rim: temperature left, humidity right */
    s_climate = lv_obj_create(scr);
    lv_obj_remove_style_all(s_climate);
    lv_obj_set_size(s_climate, SCREEN_W, RIM_H);
    lv_obj_set_pos(s_climate, 0, SCREEN_H - RIM_H);
    lv_obj_remove_flag(s_climate, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *left = climate_group(s_climate, &s_temp, &s_temp_unit);
    lv_obj_align(left, LV_ALIGN_BOTTOM_LEFT, M + 8, -5);
    lv_obj_t *right = climate_group(s_climate, &s_rh, &s_rh_unit);
    lv_obj_align(right, LV_ALIGN_BOTTOM_RIGHT, -(M + 8), -5);
    lv_label_set_text(s_temp_unit, "\xC2\xB0" "C");
    lv_label_set_text(s_rh_unit, "%");
}

static bool comma_decimal(void)
{
    i18n_locale_t l = i18n_locale();
    return l == I18N_LOCALE_DE || l == I18N_LOCALE_ES;
}

void ui_home_watch_tick(bool idle, bool muted, const char *chip)
{
    if (!s_date) return;
    kbx_set_hidden(s_climate, !idle);

    if (s_epoch != i18n_epoch()) {
        s_epoch = i18n_epoch();
        i18n_style_label(s_date, I18N_FONT_18);
    }

    char buf[48];
    time_t now = time(NULL);
    if (now >= 1700000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(buf, sizeof(buf), "%02d", tm.tm_hour);
        kbx_set_text(s_hh, buf);
        snprintf(buf, sizeof(buf), "%02d", tm.tm_min);
        kbx_set_text(s_mm, buf);
        switch (i18n_locale()) {
        case I18N_LOCALE_DE: strftime(buf, sizeof(buf), "%d.%m.%Y", &tm); break;
        case I18N_LOCALE_ES: strftime(buf, sizeof(buf), "%d/%m/%Y", &tm); break;
        default: strftime(buf, sizeof(buf), "%Y-%m-%d", &tm); break;
        }
    } else {
        kbx_set_text(s_hh, "");
        kbx_set_text(s_mm, "");
        buf[0] = '\0';
    }
    kbx_set_text(s_date, chip ? chip : buf);

    if (idle) {
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
    }

    /* status symbols, right-aligned left of the Wi-Fi symbol: mute, battery */
    const bool wifi = net_wifi_is_connected(NULL, 0);
    lv_obj_set_style_opa(s_wifi, wifi ? LV_OPA_COVER : LV_OPA_40, 0);
    char st[40] = "";
    const int pct = battery_percent();
    if (muted) strlcat(st, LV_SYMBOL_MUTE "  ", sizeof(st));
    if (pct >= 0) {
        const char *sym = pct >= 85 ? LV_SYMBOL_BATTERY_FULL : pct >= 60 ? LV_SYMBOL_BATTERY_3
                        : pct >= 35 ? LV_SYMBOL_BATTERY_2 : pct >= 12 ? LV_SYMBOL_BATTERY_1
                        : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(buf, sizeof(buf), "%d%% %s", pct, sym);
        strlcat(st, buf, sizeof(st));
    }
    kbx_set_text(s_status, st);
    lv_obj_align_to(s_status, s_wifi, LV_ALIGN_OUT_LEFT_MID, -12, 0);
}
