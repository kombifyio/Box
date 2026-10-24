#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// Initialize the CST816S touch controller (shared system I2C on the 1.85C)
// and start a task: tap starts/stops talk, hold latches mute.
// Non-fatal: returns an error (logged by the caller) but never aborts if the
// controller is absent.
esp_err_t touch_init(void);

// Last published touch state (for the LVGL pointer indev in ui.c).
// Thread-safe to read; returns the last known position even after release
// (LVGL expects that).
void touch_get_point(int *x, int *y, bool *pressed);
bool touch_point_on_orb(int x, int y);

// Write diagnostics into the log (init status, poll/press/gesture counters,
// last coordinate). Triggered by CDC command "KBX touch?".
void touch_log_status(void);

// Bus handle the touch controller talks on (= system I2C on the 1.85C), for
// diagnostics ("KBX i2cscan"); NULL until touch_init has run.
i2c_master_bus_handle_t touch_i2c_bus(void);

// Child lock / touch lock: when set, the touch task ignores all gestures
// EXCEPT the triple-tap that unlocks again. Voice/host stay untouched.
// touch_set_locked is called from the settings panel (LVGL context, just a
// bool store); touch_is_locked is read by the UI in the tick.
bool touch_is_locked(void);
void touch_set_locked(bool locked);
