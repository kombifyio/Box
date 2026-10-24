#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCM_DSP_INPUT_RATE_HZ 48000U
#define PCM_DSP_OUTPUT_RATE_HZ 16000U
#define PCM_DSP_INPUT_FRAMES_20MS 960U
#define PCM_DSP_OUTPUT_SAMPLES_20MS 320U
#define PCM_DSP_OUTPUT_BYTES_20MS (PCM_DSP_OUTPUT_SAMPLES_20MS * sizeof(int16_t))
#define PCM_DSP_FIR_TAPS 63U
#define PCM_DSP_DEFAULT_GAIN_Q8 (14U * 256U)

typedef enum {
    PCM_DSP_CHANNEL_UNSELECTED = 0,
    PCM_DSP_CHANNEL_LEFT,
    PCM_DSP_CHANNEL_RIGHT,
    PCM_DSP_CHANNEL_MIX,
} pcm_dsp_channel_t;

typedef struct {
    pcm_dsp_channel_t channel;
    bool valid;
    int32_t dc_estimate;
    uint32_t input_peak;
    uint32_t output_peak;
    uint32_t gain_clip_samples;
    uint32_t output_clip_samples;
    uint64_t total_input_frames;
    uint64_t total_output_samples;
} pcm_dsp_telemetry_t;

typedef struct {
    pcm_dsp_channel_t channel;
    bool valid;
    bool dc_initialized;
    uint32_t gain_q8;
    int64_t dc_q16;
    int16_t fir_history[PCM_DSP_FIR_TAPS];
    size_t fir_position;
    uint8_t decimation_phase;
    pcm_dsp_telemetry_t telemetry;
} pcm_dsp_t;

void pcm_dsp_init(pcm_dsp_t *state, uint32_t gain_q8);
/* Use a known microphone slot instead of picking the louder one from the
 * first frames (which may carry speaker output on the other slot). */
void pcm_dsp_set_channel(pcm_dsp_t *state, pcm_dsp_channel_t channel);
void pcm_dsp_invalidate(pcm_dsp_t *state);

/*
 * Processes exactly one 20 ms, 48 kHz stereo S16LE block into one 20 ms,
 * 16 kHz mono S16LE frame. Channel selection is made once on the first block
 * carrying energy and stays fixed for the lifetime of the state.
 */
bool pcm_dsp_process_20ms(pcm_dsp_t *state,
                          const int16_t *stereo,
                          size_t input_frames,
                          int16_t *mono,
                          size_t mono_capacity,
                          size_t *mono_samples,
                          pcm_dsp_telemetry_t *telemetry);

/*
 * Agent downlink (M1.4): 1:2 linear upsample 24 kHz -> 48 kHz mono S16LE.
 * out needs capacity for 2*n samples. *prev carries the last sample of the
 * previous block (no kink at block boundaries); initialize with 0 and reset
 * on flush. Returns the number of samples written (2*n).
 */
size_t pcm_dsp_upsample_2x(const int16_t *in, size_t n, int16_t *out, int16_t *prev);

/*
 * 1:3 linear upsample 16 kHz -> 48 kHz mono S16LE. out needs capacity for
 * 3*n samples. Same prev semantics as pcm_dsp_upsample_2x.
 */
size_t pcm_dsp_upsample_3x(const int16_t *in, size_t n, int16_t *out, int16_t *prev);
