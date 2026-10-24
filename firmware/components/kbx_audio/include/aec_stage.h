#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "pcm_dsp.h"

/* Echo cancellation for one 16 kHz capture consumer (aec_stage.c). */
typedef struct aec_stage aec_stage_t;

/* PSRAM; NULL when ESP-SR cannot create the AEC. */
aec_stage_t *aec_stage_create(void);
void aec_stage_destroy(aec_stage_t *stage);

/* Restart the filter for a microphone slot; the other slot is the
 * reference. Call when a capture turn starts or the slot changes. */
void aec_stage_reset(aec_stage_t *stage, pcm_dsp_channel_t mic);

/* AEC needs one microphone slot and the other as reference (not mix/auto). */
bool aec_stage_usable(pcm_dsp_channel_t mic);

/* One 20 ms, 48 kHz stereo block in, one 20 ms, 16 kHz mono frame out
 * (echo removed, gain_q8 applied after the AEC). */
bool aec_stage_process_20ms(aec_stage_t *stage, const int16_t *stereo, int16_t *mono, uint32_t gain_q8);

/* KBX aec?: mic, reference and output level of the last processed second. */
void aec_stage_log(void);

/* Linear AEC (false) or AEC + nonlinear suppression (true) for new stages. */
void aec_stage_set_type_vc(bool vc);
