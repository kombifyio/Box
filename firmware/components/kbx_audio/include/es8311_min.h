#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t es8311_min_init(i2c_master_bus_handle_t bus, uint32_t sample_rate_hz, uint8_t volume, uint8_t *out_addr);
esp_err_t es8311_min_set_volume(uint8_t volume);
esp_err_t es8311_min_mute(bool muted);
