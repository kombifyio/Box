/*
 * kombify box - display design (speechkit.box_settings.v1 `display.design`).
 *
 * Two designs live in the firmware and the account picks one per device:
 *   "classic"  the original layouts (orb stage with caption and status pill on
 *              the round board, room display and small orb on the RLCD,
 *              orb with a settings button on the C6)
 *   "watch"    the Pixel Watch layout: the Aura Orb fills
 *              the face and the secondary information sits in a wider rim
 * Defaults while nothing was chosen: round 1.85C classic, RLCD 4.2 classic,
 * C6 watch (the board passes its default to kbx_design_init).
 *
 * The screen is built once at start for the running design; a change is kept
 * in NVS and takes effect through a clean restart that the UI timer performs
 * as soon as the box is not in a conversation.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KBX_DESIGN_CLASSIC = 0,
    KBX_DESIGN_WATCH = 1,
} kbx_design_t;

/* Reads the stored choice (NVS) and fixes the running design; the board's
 * default applies while nothing was stored. Idempotent: the first call wins, so
 * every user of the choice may call it before reading it. */
void kbx_design_init(kbx_design_t board_default);
/* The design this boot runs. */
kbx_design_t kbx_design(void);
/* The design the settings hold: what runs after the next start. */
kbx_design_t kbx_design_stored(void);
/* Stores the choice. A different one than the running design asks for a restart. */
esp_err_t kbx_design_set(kbx_design_t design);
/* Forgets the choice: the board default applies again (a restart if it differs). */
esp_err_t kbx_design_clear(void);
/* True once the account or the console picked a design (and not just the
 * board default applies): only then do the settings documents carry
 * display.design, so a box that never chose one writes the same documents as
 * before the setting existed. */
bool kbx_design_chosen(void);
/* True once a stored choice differs from the running design. */
bool kbx_design_restart_needed(void);

const char *kbx_design_id(kbx_design_t design);
bool kbx_design_from_id(const char *id, kbx_design_t *out);

#ifdef __cplusplus
}
#endif
