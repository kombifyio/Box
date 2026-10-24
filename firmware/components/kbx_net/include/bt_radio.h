#pragma once
#include <stdbool.h>

/* Minimal BLE radio (NimBLE) for the Bluetooth tile in the settings panel.
 * Stage 2: radio on/off only — the box advertises as "kombify box", still
 * without a GATT service or pairing (comes with B5.3 Improv provisioning).
 *
 * Lazy + boot-safe: the NimBLE stack starts ONLY on first enable (not in
 * the boot path), and every init/advertising error is non-fatal (log + give
 * up, no abort) — a BLE problem cannot put the box into a boot loop. */
void bt_radio_init(void);            /* lightweight; does NOT start the stack */
void bt_radio_set_enabled(bool on);  /* on: lazy-start stack + advertise; off: stop advertising */
bool bt_radio_is_enabled(void);

/* Return BT-controller internal RAM to the heap (early in boot, before any
 * controller init). After this the BLE tile stays off. M1 trade: standalone
 * voice (net_ws) needs that internal RAM. */
void bt_radio_release(void);
