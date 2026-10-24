/*
 * kombify box - UI 2 widget primitives.
 *
 * Every object starts from lv_obj_remove_style_all(): no default-theme
 * padding, borders, press shadows, scrollbars or focus outlines leak in.
 * Shapes and sizes follow the mockup tokens (8 px grid, pill 34 px / radius
 * 17, button 40 px / radius 14, tile radius 16, hairline borders).
 * LVGL context only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "i18n.h"

typedef enum {
    KBX_PILL_NEUTRAL = 0,   /* #141B28 + hairline, mid text      */
    KBX_PILL_BRAND,         /* tint + tint line, hi text         */
    KBX_PILL_ALERT,         /* alert ground, alert text          */
} kbx_pill_kind_t;

typedef enum {
    KBX_BTN_TINT = 0,       /* default action (Retry, Updates)   */
    KBX_BTN_PRIMARY,        /* brand fill, dark text             */
    KBX_BTN_TEXT,           /* text only (Cancel, Close)         */
} kbx_btn_kind_t;

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *dot;          /* 9 px state dot (hidden unless set) */
    lv_obj_t *icon;         /* LV_SYMBOL glyph (hidden unless set) */
    lv_obj_t *text;
    lv_obj_t *spin[3];      /* "thinking" dots (hidden unless on)  */
    kbx_pill_kind_t kind;
    bool spinning;
} kbx_pill_t;

/* Bare container: no style, not scrollable, not clickable. */
lv_obj_t *kbx_box(lv_obj_t *parent);

/* Full-screen opaque sheet on the screen ground (pairing, update, settings).
 * Opaque so LVGL skips the scene underneath (cover check). Hidden. */
lv_obj_t *kbx_sheet(lv_obj_t *parent);

/* Label in the i18n font of `size`, centred text, given colour. */
lv_obj_t *kbx_label(lv_obj_t *parent, i18n_font_size_t size, uint32_t color);

/* Wrapped, centred label of fixed width. */
lv_obj_t *kbx_text_block(lv_obj_t *parent, i18n_font_size_t size, uint32_t color, int width);

void kbx_pill_create(kbx_pill_t *p, lv_obj_t *parent);
void kbx_pill_set_kind(kbx_pill_t *p, kbx_pill_kind_t kind);
/* dot: 0 = none, else 0xRRGGBB. icon: NULL = none. */
void kbx_pill_set(kbx_pill_t *p, uint32_t dot, const char *icon, const char *text);
void kbx_pill_set_spinning(kbx_pill_t *p, bool on);

/* Button of `kind`; label styled with I18N_FONT_16. */
lv_obj_t *kbx_button(lv_obj_t *parent, kbx_btn_kind_t kind, int width,
                     lv_event_cb_t cb, lv_obj_t **label_out);
void kbx_button_set_kind(lv_obj_t *btn, lv_obj_t *label, kbx_btn_kind_t kind);

/* Edge ring: lv_arc of outer diameter d and stroke w, rounded ends, track in
 * the ring-track colour, no knob, not clickable. Centred on the parent. */
lv_obj_t *kbx_ring(lv_obj_t *parent, int d, int w);

/* Hide/show without invalidating when nothing changes. */
void kbx_set_hidden(lv_obj_t *obj, bool hidden);

/* Set label text only when it differs (avoids redraws every tick). */
void kbx_set_text(lv_obj_t *label, const char *text);
