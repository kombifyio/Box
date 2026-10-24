/*
 * kombify box - UI 2 widget primitives. See ui_style.h.
 */
#include "ui_style.h"

#include <string.h>

#include "ui_theme.h"

#define PILL_H        34
#define PILL_PAD_X    16
#define PILL_GAP      8
#define PILL_DOT      9
#define PILL_TEXT_MAX 196   /* widest text that keeps the pill inside the bottom chord */
#define BTN_H         40
#define BTN_R         14
#define SPIN_DOT      5
#define SPIN_PERIOD   600   /* ms per half cycle (1.2 s loop, mockup) */

static lv_color_t c(uint32_t hex) { return lv_color_hex(hex); }

lv_obj_t *kbx_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                          LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    return o;
}

lv_obj_t *kbx_sheet(lv_obj_t *parent)
{
    lv_obj_t *s = kbx_box(parent);
    lv_obj_set_size(s, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s);
    lv_obj_set_style_bg_color(s, c(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    /* A modal consumes taps on empty space so nothing underneath reacts. */
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    return s;
}

lv_obj_t *kbx_label(lv_obj_t *parent, i18n_font_size_t size, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    i18n_style_label(l, size);
    lv_obj_set_style_text_color(l, c(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *kbx_text_block(lv_obj_t *parent, i18n_font_size_t size, uint32_t color, int width)
{
    lv_obj_t *l = kbx_label(parent, size, color);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(l, size >= I18N_FONT_18 ? 4 : 2, 0);
    return l;
}

/* ------------------------------------------------------------------ pill */

static void spin_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

void kbx_pill_create(kbx_pill_t *p, lv_obj_t *parent)
{
    memset(p, 0, sizeof(*p));
    p->kind = (kbx_pill_kind_t)-1;
    lv_obj_t *o = kbx_box(parent);
    p->obj = o;
    lv_obj_set_height(o, PILL_H);
    lv_obj_set_width(o, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(o, PILL_H / 2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_pad_hor(o, PILL_PAD_X, 0);
    lv_obj_set_style_pad_column(o, PILL_GAP, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    p->dot = kbx_box(o);
    lv_obj_set_size(p->dot, PILL_DOT, PILL_DOT);
    lv_obj_set_style_radius(p->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(p->dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(p->dot, LV_OBJ_FLAG_HIDDEN);

    p->icon = lv_label_create(o);
    lv_obj_remove_style_all(p->icon);
    lv_obj_set_style_text_font(p->icon, &lv_font_montserrat_16, 0);
    lv_obj_add_flag(p->icon, LV_OBJ_FLAG_HIDDEN);

    p->text = kbx_label(o, I18N_FONT_16, KBX_COL_TEXT_MID);
    lv_obj_set_style_max_width(p->text, PILL_TEXT_MAX, 0);
    lv_label_set_long_mode(p->text, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *row = kbx_box(o);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *d = kbx_box(row);
        lv_obj_set_size(d, SPIN_DOT, SPIN_DOT);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        p->spin[i] = d;
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    kbx_pill_set_kind(p, KBX_PILL_NEUTRAL);
}

void kbx_pill_set_kind(kbx_pill_t *p, kbx_pill_kind_t kind)
{
    if (p->kind == kind) return;
    p->kind = kind;
    uint32_t bg = KBX_COL_PILL, line = KBX_COL_LINE, txt = KBX_COL_TEXT_MID, ic = 0x9AA3B5;
    if (kind == KBX_PILL_BRAND) {
        bg = KBX_COL_TINT; line = KBX_COL_TINT_LINE; txt = KBX_COL_TEXT_HI; ic = KBX_COL_BRAND;
    } else if (kind == KBX_PILL_ALERT) {
        bg = KBX_COL_ALERT_BG; line = KBX_COL_ALERT_LINE; txt = KBX_COL_ALERT; ic = KBX_COL_ALERT;
    }
    lv_obj_set_style_bg_color(p->obj, c(bg), 0);
    lv_obj_set_style_border_color(p->obj, c(line), 0);
    lv_obj_set_style_text_color(p->text, c(txt), 0);
    lv_obj_set_style_text_color(p->icon, c(ic), 0);
    for (int i = 0; i < 3; i++) lv_obj_set_style_bg_color(p->spin[i], c(txt), 0);
}

void kbx_pill_set(kbx_pill_t *p, uint32_t dot, const char *icon, const char *text)
{
    kbx_set_hidden(p->dot, dot == 0);
    if (dot) {
        lv_color_t dc = c(dot);
        if (!lv_color_eq(lv_obj_get_style_bg_color(p->dot, 0), dc)) {
            lv_obj_set_style_bg_color(p->dot, dc, 0);
        }
    }
    kbx_set_hidden(p->icon, icon == NULL);
    if (icon) kbx_set_text(p->icon, icon);
    kbx_set_text(p->text, text);
}

void kbx_pill_set_spinning(kbx_pill_t *p, bool on)
{
    if (p->spinning == on) return;
    p->spinning = on;
    lv_obj_t *row = lv_obj_get_parent(p->spin[0]);
    kbx_set_hidden(row, !on);
    for (int i = 0; i < 3; i++) {
        lv_anim_delete(p->spin[i], spin_anim_cb);
        if (!on) continue;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, p->spin[i]);
        lv_anim_set_exec_cb(&a, spin_anim_cb);
        lv_anim_set_values(&a, 64, 255);
        lv_anim_set_duration(&a, SPIN_PERIOD);
        lv_anim_set_reverse_duration(&a, SPIN_PERIOD);
        lv_anim_set_delay(&a, (uint32_t)(i * 200));
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

/* ---------------------------------------------------------------- button */

void kbx_button_set_kind(lv_obj_t *b, lv_obj_t *label, kbx_btn_kind_t kind)
{
    uint32_t bg = KBX_COL_TINT, line = KBX_COL_TINT_LINE, txt = KBX_COL_TEXT_HI;
    lv_opa_t bg_opa = LV_OPA_COVER;
    int bw = 1;
    if (kind == KBX_BTN_PRIMARY) {
        bg = KBX_COL_BRAND; line = KBX_COL_BRAND; txt = KBX_COL_BG;
    } else if (kind == KBX_BTN_TEXT) {
        bg_opa = LV_OPA_TRANSP; bw = 0; txt = KBX_COL_TEXT_MID;
    }
    lv_obj_set_style_bg_color(b, c(bg), 0);
    lv_obj_set_style_bg_opa(b, bg_opa, 0);
    lv_obj_set_style_border_color(b, c(line), 0);
    lv_obj_set_style_border_width(b, bw, 0);
    if (label) lv_obj_set_style_text_color(label, c(txt), 0);
}

lv_obj_t *kbx_button(lv_obj_t *parent, kbx_btn_kind_t kind, int width,
                     lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(b, width, BTN_H);
    lv_obj_set_style_radius(b, BTN_R, 0);
    /* Pressed feedback: a short opacity dip, no shadow, no transform. */
    lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = kbx_label(b, I18N_FONT_16, KBX_COL_TEXT_HI);
    lv_obj_set_width(l, width - 16);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(l);
    kbx_button_set_kind(b, l, kind);
    if (label_out) *label_out = l;
    return b;
}

/* ------------------------------------------------------------------ ring */

lv_obj_t *kbx_ring(lv_obj_t *parent, int d, int w)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, d, d);
    lv_obj_center(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, c(KBX_COL_RING_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    return a;
}

/* ----------------------------------------------------------------- utils */

void kbx_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

void kbx_set_text(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}
