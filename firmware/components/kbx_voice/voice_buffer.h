#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "pcm_dsp.h"

/* 32 seconds covers account renewal, canonical history and media startup.
 * Never overwrite an earlier utterance.
 * Caller serializes access; invalidation scrubs all retained microphone PCM. */
#define KBX_VOICE_BUFFER_FRAMES 1600U
typedef struct {
    int16_t frames[KBX_VOICE_BUFFER_FRAMES][PCM_DSP_OUTPUT_SAMPLES_20MS];
    size_t head;
    size_t count;
    bool valid;
} kbx_voice_buffer_t;

static inline void kbx_voice_buffer_reset(kbx_voice_buffer_t *b, bool valid)
{
    memset(b, 0, sizeof(*b));
    b->valid = valid;
}

static inline bool kbx_voice_buffer_push(kbx_voice_buffer_t *b, const int16_t *frame)
{
    if (!b->valid) return false;
    if (b->count == KBX_VOICE_BUFFER_FRAMES) {
        kbx_voice_buffer_reset(b, false);
        return false;
    }
    size_t tail = (b->head + b->count) % KBX_VOICE_BUFFER_FRAMES;
    memcpy(b->frames[tail], frame, PCM_DSP_OUTPUT_BYTES_20MS);
    ++b->count;
    return true;
}

static inline bool kbx_voice_buffer_pop(kbx_voice_buffer_t *b, int16_t *frame)
{
    if (!b->valid || b->count == 0) return false;
    memcpy(frame, b->frames[b->head], PCM_DSP_OUTPUT_BYTES_20MS);
    memset(b->frames[b->head], 0, PCM_DSP_OUTPUT_BYTES_20MS);
    b->head = (b->head + 1U) % KBX_VOICE_BUFFER_FRAMES;
    --b->count;
    return true;
}
