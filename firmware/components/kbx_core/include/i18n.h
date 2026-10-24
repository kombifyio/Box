/*
 * kombify box - display localisation.
 *
 * Every customer-visible string on the display is a message ID (i18n_ids.h,
 * generated from firmware/i18n/messages.json by firmware/i18n/gen.mjs) that is
 * resolved with i18n_str(). The current locale lives in NVS (namespace
 * "kbx_ui", key "locale") and is set through the device's existing config
 * channels:
 *   - USB CDC:             KBX locale <tag> | KBX locale?      (usb_log.c)
 *   - session-create JSON: optional "locale" field, applied on every
 *                          successful session start (net_ws.c)
 * Default is English. Tags are BCP 47 style and case-insensitive: en, de, es,
 * zh-Hans (also zh, zh-CN, zh-SG), ar, hi (region subtags are ignored).
 * Unsupported languages resolve to English.
 *
 * Rendering per locale:
 *   en/de/es  Montserrat (LVGL built-in) plus a small accented-letter subset.
 *   zh-Hans   Noto Sans SC subset generated for exactly the strings in use.
 *   ar        Noto Sans Arabic presentation forms; LVGL Arabic/Persian
 *             shaping (LV_USE_ARABIC_PERSIAN_CHARS) and BiDi (LV_USE_BIDI).
 *   hi        NOT rendered. LVGL's basic text engine cannot shape Devanagari
 *             (no conjunct forms, no pre-base i-matra reordering), so unshaped
 *             Hindi would be wrong text. The requested tag "hi" is stored and
 *             reported, but the display shows English. Proper Hindi needs a
 *             shaping engine (HarfBuzz via LV_USE_FREETYPE), which costs far
 *             more RAM and flash than this device can spare.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "i18n_ids.h"

typedef enum {
    I18N_LOCALE_EN = 0,
    I18N_LOCALE_DE,
    I18N_LOCALE_ES,
    I18N_LOCALE_ZH_HANS,
    I18N_LOCALE_AR,
    I18N_LOCALE_HI,        /* requested only; rendered as English */
} i18n_locale_t;

/* UI 2 type ramp (S size class): 14 small, 16 body and pills, 18 captions
 * and titles, 20 the error line. */
typedef enum {
    I18N_FONT_14 = 0,
    I18N_FONT_16,
    I18N_FONT_18,
    I18N_FONT_20,
} i18n_font_size_t;

/* Load the stored locale. Safe to call before net_wifi_init (initialises NVS
 * itself; failures leave English). */
void i18n_init(void);

/* Set + persist the locale from a tag. Unsupported languages become English.
 * ESP_ERR_INVALID_ARG for NULL/empty. Callable from any task. */
esp_err_t i18n_set_locale_tag(const char *tag);

/* Locale the UI actually renders (hi collapses to en) and the one requested. */
i18n_locale_t i18n_locale(void);
i18n_locale_t i18n_requested_locale(void);
const char *i18n_locale_tag(i18n_locale_t locale);

/* Localised string; never NULL (falls back to English). */
const char *i18n_str(i18n_id_t id);

/* Font that has the glyphs of the effective locale (falls back to Montserrat
 * for digits, ASCII and the LV_SYMBOL icons). */
const lv_font_t *i18n_font(i18n_font_size_t size);

/* Bumps whenever the effective locale changes; UI timers compare it to their
 * last value and restyle their labels (LVGL context only). */
uint32_t i18n_epoch(void);

/* Set font + text direction of a label for the effective locale. LVGL context
 * only (call with the port lock held or from an LVGL timer). */
void i18n_style_label(lv_obj_t *label, i18n_font_size_t size);

void i18n_log_status(void);
