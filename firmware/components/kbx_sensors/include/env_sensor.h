#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Room climate (env_sensor.c): SHTC3 on the system I2C bus. Non-fatal when
 * the board has no sensor. */
esp_err_t env_sensor_init(void);

/* Latest reading (temperature incl. the stored offset); false before the
 * first successful measurement or without a sensor. Any task. */
bool env_sensor_get(float *temp_c, float *rh);

/* CDC/LAN "KBX env?" and "KBX env offset <tenths>". */
void env_sensor_cdc_command(const char *args);
