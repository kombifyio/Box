/*
 * kombify box C6 - portrait UI for the 368x448 AMOLED.
 *
 * Same visual language as the S3 Box (ui_theme.h colours, Aura state
 * palette, kombify rosette, Montserrat + kbx_lat fonts), drawn with plain
 * LVGL widgets instead of the S3's PSRAM-baked orb layers.
 */
#pragma once
#include "esp_err.h"

esp_err_t c6_ui_start(void);
/* KBX settings end|confirm: the settings scrolled to the end of the list, or
 * with the power-off confirmation open (for screenshots). */
void c6_ui_show_settings_at(const char *where);
/* Open the account pairing sheet as if the user had tapped Connect. */
void c6_ui_show_pairing(void);
/* Open the settings sheet (KBX settings). */
void c6_ui_show_settings(void);
/* KBX swipe: run the top-edge swipe decision on a synthetic stroke. */
void c6_ui_swipe_selftest(void);
/* Redraw the screen once and stream it over USB (KBX shot). */
void c6_ui_screenshot(void);
