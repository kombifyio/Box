#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* RLCD 4.2 home screen (ui_home.c): clock, date, room climate, small mark.
 * Built after the voice scene so it covers the idle stage. LVGL context. */
void ui_home_build(lv_obj_t *scr);

/* Each scene tick: show = no voice turn, error or info line is on screen. */
void ui_home_tick(bool show, bool muted);
