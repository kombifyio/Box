#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t es7210_min_init(i2c_master_bus_handle_t bus, uint32_t sample_rate_hz, uint8_t *out_addr);

/* Call before init: TDM puts all four ADCs on SDOUT1 (MIC1, MIC2, MIC3, MIC4). */
void es7210_min_use_tdm(bool tdm);

/* Diagnostics (KBX es7210 <reg> [value]): value >= 0 writes, else reads into out. */
esp_err_t es7210_min_reg(uint8_t reg, int value, uint8_t *out);
