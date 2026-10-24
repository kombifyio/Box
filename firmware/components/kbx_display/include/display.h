#pragma once
#include "esp_err.h"
esp_err_t display_init(void);
void display_log_status(void);

/* Copy the panel frame as sent to the display (RLCD: 400x300, 1 bpp, panel
 * packing, set bit = white). Returns the byte count, 0 when the board has
 * no frame copy. Any task. */
#include <stddef.h>
#include <stdint.h>
size_t display_frame_copy(uint8_t *out, size_t cap);
