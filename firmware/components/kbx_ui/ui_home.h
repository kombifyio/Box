#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* RLCD 4.2 home screen (ui_home.c): clock, date, room climate, small mark.
 * Built after the voice scene so it covers the idle stage. LVGL context. */
void ui_home_build(lv_obj_t *scr);

/* Each scene tick: show = no voice turn, error or info line is on screen. */
void ui_home_tick(bool show, bool muted);

/* The watch design (ui_home_watch.c): two solid rims instead of the room
 * display. Built right after the orb, so it covers the orb's edge and sits
 * under every other widget. idle = no voice turn, error, info line or pill is
 * on screen (the bottom rim shows the room climate; otherwise a pill takes it
 * over); chip = a word that replaces the date in the top rim (the
 * conversation), or NULL. LVGL context. */
void ui_home_watch_build(lv_obj_t *scr);
void ui_home_watch_tick(bool idle, bool muted, const char *chip);
