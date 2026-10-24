#pragma once
#include "esp_err.h"

/* Battery gauge (battery.c): boards with BAT_ADC_GPIO (RLCD 4.2). */
esp_err_t battery_init(void);

/* Latest cell voltage in mV and level 0..100 %; -1 without a reading. */
int battery_mv(void);
int battery_percent(void);

/* KBX bat? */
void battery_log(void);
