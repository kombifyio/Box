/*
 * kombify box C6 - power off through the AXP2101 PMU.
 *
 * The board runs from the PMU's DCDC rails, also on USB power. Software
 * power-off (REG 10h bit 0) cuts them; a short press on the PWR key turns
 * the device on again.
 */
#pragma once
#include "esp_err.h"

/* Turns the device off. Returns only when the PMU did not accept the
 * command; the device then sleeps with the screen off instead. */
esp_err_t c6_power_off(void);
