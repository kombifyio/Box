#pragma once

// I2C diagnostics (CDC: "KBX i2cscan" / "KBX tpreset"), run asynchronously
// in a short-lived task and log results (tag "kbx_diag").
void diag_i2c_scan_async(void);
void diag_tp_reset_async(void);
