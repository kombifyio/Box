/*
 * kombify box - Aura Orb: LVGL parity port of the SpeechKit Voice UI Kit
 * assistant visual (`speechkit-voice-assistant`, "Aura Orb").
 *
 * SSOT: SpeechKit clients/typescript/packages/voice-ui
 *   - spec/voice-ui.spec.md (behaviour)
 *   - src/tokens/tokens.json `assistant` block (layer stack, per-state colour
 *     pairs aura-<state>-a/-b, animation periods, level formulas)
 * tokens.json is the arbiter; change there first. Worked native-port example:
 * android/voice-ui-compose/.../VoiceAuraOrb.kt (version speechkit.voice_ui.v1,
 * synced 2026-08-21).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "display_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AURA_INACTIVE = 0,
    AURA_CONNECTING,
    AURA_LISTENING,
    AURA_PROCESSING,
    AURA_SPEAKING,
    AURA_RECOVERING,
    AURA_SETTLING,
    AURA_ERROR,
} aura_state_t;

/* Build the orb on parent at the profile's stage (centre + diameter). Must
 * run under the held LVGL port lock; bakes the gradient layers once into
 * PSRAM. The centre mark is the kombify rosette (never recoloured). */
esp_err_t aura_orb_create(lv_obj_t *parent, const kbx_display_profile_t *profile);

/* State change: palette swap + rest/alive mode (tokens.json states). */
void aura_orb_set_state(aura_state_t state);

/* Raw level 0..100; smoothing (attack 0.55/tick, release exp 5/s) is internal. */
void aura_orb_set_level(uint8_t level_0_100);

/* Pause level updates (panel open = scene covered). */
void aura_orb_set_paused(bool paused);

/* Muted chrome (R-7): rings and glow fade to .15, rosette to .40. Mute is not
 * an Aura state; the orb keeps its (resting) state underneath. */
void aura_orb_set_dim(bool dim);

/* Hide the whole orb (the RLCD home screen covers the idle stage). */
void aura_orb_set_hidden(bool hidden);

bool aura_orb_paused(void);
bool aura_orb_anims_running(void);
aura_state_t aura_orb_state(void);

/* Lead color (aura-<state>-a) for chrome outside the orb (arc, band). */
lv_color_t aura_orb_lead_color(aura_state_t state);

#ifdef __cplusplus
}
#endif
