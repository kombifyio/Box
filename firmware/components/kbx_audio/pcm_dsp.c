#include "pcm_dsp.h"

#include <limits.h>
#include <string.h>

/*
 * 63-tap Blackman-windowed low-pass, 6.5 kHz cutoff at 48 kHz, Q15.
 * The stop band is below -37 dB at the 8 kHz output Nyquist frequency and
 * drops below -64 dB by 8.5 kHz. Coefficients sum to exactly 32768.
 */
static const int16_t k_fir_q15[PCM_DSP_FIR_TAPS] = {
    0, 0, -1, -3, -5, -1, 10, 23, 21, -6, -50, -76, -45, 50, 156, 178,
    50, -190, -381, -328, 37, 537, 802, 496, -389, -1378, -1683, -631,
    1860, 5084, 7809, 8876, 7809, 5084, 1860, -631, -1683, -1378, -389,
    496, 802, 537, 37, -328, -381, -190, 50, 178, 156, 50, -45, -76,
    -50, -6, 21, 23, 10, -1, -5, -3, -1, 0, 0,
};

static uint32_t abs_i32(int32_t value)
{
    if (value >= 0) {
        return (uint32_t)value;
    }
    return (uint32_t)(-(int64_t)value);
}

static int16_t clamp_s16(int64_t value, uint32_t *clips)
{
    if (value > INT16_MAX) {
        if (clips) {
            (*clips)++;
        }
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        if (clips) {
            (*clips)++;
        }
        return INT16_MIN;
    }
    return (int16_t)value;
}

static pcm_dsp_channel_t select_channel(const int16_t *stereo, size_t frames)
{
    int64_t sum_left = 0;
    int64_t sum_right = 0;
    for (size_t i = 0; i < frames; ++i) {
        sum_left += stereo[2 * i];
        sum_right += stereo[2 * i + 1];
    }

    const int32_t mean_left = (int32_t)(sum_left / (int64_t)frames);
    const int32_t mean_right = (int32_t)(sum_right / (int64_t)frames);
    uint64_t energy_left = 0;
    uint64_t energy_right = 0;
    for (size_t i = 0; i < frames; ++i) {
        const int32_t left = (int32_t)stereo[2 * i] - mean_left;
        const int32_t right = (int32_t)stereo[2 * i + 1] - mean_right;
        energy_left += (uint64_t)((int64_t)left * left);
        energy_right += (uint64_t)((int64_t)right * right);
    }

    if (energy_left == 0 && energy_right == 0) {
        return PCM_DSP_CHANNEL_UNSELECTED;
    }
    if (energy_right == 0 || energy_left / 16U > energy_right) {
        return PCM_DSP_CHANNEL_LEFT;
    }
    if (energy_left == 0 || energy_right / 16U > energy_left) {
        return PCM_DSP_CHANNEL_RIGHT;
    }
    return PCM_DSP_CHANNEL_MIX;
}

static int32_t select_sample(pcm_dsp_channel_t channel, int16_t left, int16_t right)
{
    if (channel == PCM_DSP_CHANNEL_LEFT) {
        return left;
    }
    if (channel == PCM_DSP_CHANNEL_RIGHT) {
        return right;
    }
    return ((int32_t)left + (int32_t)right) / 2;
}

static int32_t initial_dc(const pcm_dsp_t *state, const int16_t *stereo, size_t frames)
{
    int64_t sum = 0;
    for (size_t i = 0; i < frames; ++i) {
        sum += select_sample(state->channel, stereo[2 * i], stereo[2 * i + 1]);
    }
    return (int32_t)(sum / (int64_t)frames);
}

void pcm_dsp_init(pcm_dsp_t *state, uint32_t gain_q8)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->valid = true;
    state->gain_q8 = gain_q8 == 0 ? 256U : gain_q8;
    state->telemetry.valid = true;
}

void pcm_dsp_set_channel(pcm_dsp_t *state, pcm_dsp_channel_t channel)
{
    if (!state) {
        return;
    }
    state->channel = channel;
    state->telemetry.channel = channel;
    state->dc_initialized = false;
}

void pcm_dsp_invalidate(pcm_dsp_t *state)
{
    if (!state) {
        return;
    }
    state->valid = false;
    state->telemetry.valid = false;
}

bool pcm_dsp_process_20ms(pcm_dsp_t *state,
                          const int16_t *stereo,
                          size_t input_frames,
                          int16_t *mono,
                          size_t mono_capacity,
                          size_t *mono_samples,
                          pcm_dsp_telemetry_t *telemetry)
{
    if (mono_samples) {
        *mono_samples = 0;
    }
    if (!state || !state->valid || !stereo || !mono || !mono_samples ||
        input_frames != PCM_DSP_INPUT_FRAMES_20MS ||
        mono_capacity < PCM_DSP_OUTPUT_SAMPLES_20MS) {
        pcm_dsp_invalidate(state);
        if (telemetry && state) {
            *telemetry = state->telemetry;
        }
        return false;
    }

    if (state->channel == PCM_DSP_CHANNEL_UNSELECTED) {
        const pcm_dsp_channel_t selected = select_channel(stereo, input_frames);
        if (selected != PCM_DSP_CHANNEL_UNSELECTED) {
            state->channel = selected;
            state->telemetry.channel = selected;
            state->dc_initialized = false;
        }
    }
    if (!state->dc_initialized) {
        state->dc_q16 = (int64_t)initial_dc(state, stereo, input_frames) << 16;
        state->dc_initialized = true;
    }

    uint32_t frame_input_peak = 0;
    uint32_t frame_output_peak = 0;
    size_t produced = 0;

    for (size_t i = 0; i < input_frames; ++i) {
        const int32_t selected = select_sample(state->channel,
                                               stereo[2 * i],
                                               stereo[2 * i + 1]);
        const int64_t target_q16 = (int64_t)selected << 16;
        state->dc_q16 += (target_q16 - state->dc_q16) / 4096;
        const int32_t centered = selected - (int32_t)(state->dc_q16 >> 16);
        const uint32_t centered_abs = abs_i32(centered);
        if (centered_abs > frame_input_peak) {
            frame_input_peak = centered_abs;
        }

        int64_t gained = (int64_t)centered * state->gain_q8;
        gained = gained >= 0 ? (gained + 128) / 256 : (gained - 128) / 256;
        const int16_t filtered_input = clamp_s16(gained, &state->telemetry.gain_clip_samples);
        state->fir_history[state->fir_position] = filtered_input;

        if (state->decimation_phase == 0) {
            int64_t acc = 0;
            size_t history_index = state->fir_position;
            for (size_t tap = 0; tap < PCM_DSP_FIR_TAPS; ++tap) {
                acc += (int32_t)state->fir_history[history_index] * k_fir_q15[tap];
                history_index = history_index == 0 ? PCM_DSP_FIR_TAPS - 1 : history_index - 1;
            }
            const int64_t filtered = acc >= 0
                                         ? (acc + (1LL << 14)) / (1LL << 15)
                                         : -((-acc + (1LL << 14)) / (1LL << 15));
            const int16_t output = clamp_s16(filtered,
                                             &state->telemetry.output_clip_samples);
            mono[produced++] = output;
            const uint32_t output_abs = abs_i32(output);
            if (output_abs > frame_output_peak) {
                frame_output_peak = output_abs;
            }
        }

        state->fir_position++;
        if (state->fir_position == PCM_DSP_FIR_TAPS) {
            state->fir_position = 0;
        }
        state->decimation_phase++;
        if (state->decimation_phase == 3) {
            state->decimation_phase = 0;
        }
    }

    if (produced != PCM_DSP_OUTPUT_SAMPLES_20MS) {
        pcm_dsp_invalidate(state);
        if (telemetry) {
            *telemetry = state->telemetry;
        }
        return false;
    }

    state->telemetry.valid = true;
    state->telemetry.dc_estimate = (int32_t)(state->dc_q16 >> 16);
    state->telemetry.input_peak = frame_input_peak;
    state->telemetry.output_peak = frame_output_peak;
    state->telemetry.total_input_frames += input_frames;
    state->telemetry.total_output_samples += produced;
    *mono_samples = produced;
    if (telemetry) {
        *telemetry = state->telemetry;
    }
    return true;
}

size_t pcm_dsp_upsample_2x(const int16_t *in, size_t n, int16_t *out, int16_t *prev)
{
    if (!in || !out || !prev) {
        return 0;
    }
    int16_t last = *prev;
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = (int16_t)(((int32_t)last + (int32_t)in[i]) / 2);
        out[2 * i + 1] = in[i];
        last = in[i];
    }
    *prev = last;
    return 2 * n;
}

size_t pcm_dsp_upsample_3x(const int16_t *in, size_t n, int16_t *out, int16_t *prev)
{
    if (!in || !out || !prev) {
        return 0;
    }
    int16_t last = *prev;
    for (size_t i = 0; i < n; i++) {
        int32_t a = last;
        int32_t b = in[i];
        out[3 * i] = (int16_t)((2 * a + b) / 3);
        out[3 * i + 1] = (int16_t)((a + 2 * b) / 3);
        out[3 * i + 2] = in[i];
        last = in[i];
    }
    *prev = last;
    return 3 * n;
}
