/*
 * IMA ADPCM, 4 bits per 16-bit sample. The C6 has no PSRAM, so the speech
 * captured between the wake word (or tap) and the server's listening
 * acknowledgement is held 4:1 compressed: 32 KB keep about 4 s of 16 kHz
 * audio. It is decoded back to S16LE before it leaves the device; the
 * uplink contract stays plain PCM.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t predictor;
    int32_t index;
} c6_adpcm_t;

static const int16_t C6_ADPCM_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t C6_ADPCM_INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline void c6_adpcm_reset(c6_adpcm_t *s)
{
    s->predictor = 0;
    s->index = 0;
}

/* Shared step: applies a 4-bit code to the state and returns the new sample. */
static inline int16_t c6_adpcm_apply(c6_adpcm_t *s, uint8_t code)
{
    int32_t step = C6_ADPCM_STEP[s->index];
    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    s->predictor += (code & 8) ? -diff : diff;
    if (s->predictor > 32767) s->predictor = 32767;
    if (s->predictor < -32768) s->predictor = -32768;
    s->index += C6_ADPCM_INDEX[code & 15];
    if (s->index < 0) s->index = 0;
    if (s->index > 88) s->index = 88;
    return (int16_t)s->predictor;
}

static inline uint8_t c6_adpcm_code(c6_adpcm_t *s, int16_t sample)
{
    int32_t step = C6_ADPCM_STEP[s->index];
    int32_t diff = (int32_t)sample - s->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) { code |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) code |= 1;
    c6_adpcm_apply(s, code);
    return code;
}

/* samples must be even; out receives samples / 2 bytes (low nibble first). */
static inline void c6_adpcm_encode(c6_adpcm_t *s, const int16_t *in, size_t samples, uint8_t *out)
{
    for (size_t i = 0; i + 1 < samples; i += 2) {
        uint8_t lo = c6_adpcm_code(s, in[i]);
        uint8_t hi = c6_adpcm_code(s, in[i + 1]);
        *out++ = (uint8_t)(lo | (hi << 4));
    }
}

static inline void c6_adpcm_decode(c6_adpcm_t *s, const uint8_t *in, size_t bytes, int16_t *out)
{
    for (size_t i = 0; i < bytes; ++i) {
        *out++ = c6_adpcm_apply(s, in[i] & 15);
        *out++ = c6_adpcm_apply(s, in[i] >> 4);
    }
}
