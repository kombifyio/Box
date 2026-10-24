#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cJSON.h"

/* Boards without PSRAM (ESP32-C6 variant) pass a smaller bound. */
#ifndef KBX_VOICE_TEXT_LIMIT
#define KBX_VOICE_TEXT_LIMIT (64U * 1024U)
#endif

/* A message may span websocket continuation frames, and each frame may be
 * delivered in several SDK callbacks. Reject gaps, overlap and oversize
 * before parsing any control metadata. Caller serializes access. */
typedef struct {
    char bytes[KBX_VOICE_TEXT_LIMIT + 1U];
    size_t used;
    size_t frame_length;
    size_t frame_used;
    uint8_t frame_opcode;
    bool frame_final;
    bool frame_open;
    bool active;
} kbx_voice_text_t;

typedef enum {
    KBX_VOICE_TEXT_INVALID = -1,
    KBX_VOICE_TEXT_PENDING = 0,
    KBX_VOICE_TEXT_COMPLETE = 1,
} kbx_voice_text_result_t;

static inline void kbx_voice_text_reset(kbx_voice_text_t *b)
{
    memset(b, 0, sizeof(*b));
}

static inline kbx_voice_text_result_t kbx_voice_text_feed(kbx_voice_text_t *b,
    uint8_t opcode, bool final, size_t payload_length, size_t offset,
    const char *data, size_t length)
{
    if (offset == 0) {
        if (b->frame_open) goto invalid;
        if (opcode == 1U) {
            if (b->active) goto invalid;
            b->used = 0;
            b->active = true;
        } else if (opcode != 0U || !b->active) {
            goto invalid;
        }
        if (payload_length > KBX_VOICE_TEXT_LIMIT - b->used) goto invalid;
        b->frame_length = payload_length;
        b->frame_used = 0;
        b->frame_opcode = opcode;
        b->frame_final = final;
        b->frame_open = true;
    }
    if (!b->active || !b->frame_open || opcode != b->frame_opcode ||
        final != b->frame_final || payload_length != b->frame_length ||
        offset != b->frame_used || offset > payload_length ||
        length > payload_length - offset || length > KBX_VOICE_TEXT_LIMIT - b->used ||
        (length && (!data || memchr(data, '\0', length)))) goto invalid;
    if (length) memcpy(b->bytes + b->used, data, length);
    b->used += length;
    b->frame_used += length;
    if (b->frame_used != b->frame_length) return KBX_VOICE_TEXT_PENDING;
    b->frame_open = false;
    if (!final) return KBX_VOICE_TEXT_PENDING;
    b->bytes[b->used] = '\0';
    b->active = false;
    return KBX_VOICE_TEXT_COMPLETE;

invalid:
    kbx_voice_text_reset(b);
    return KBX_VOICE_TEXT_INVALID;
}

enum {
    KBX_VOICE_INPUT_HEARD = 1U << 0,
    KBX_VOICE_INPUT_FINAL = 1U << 1,
    KBX_VOICE_TURN_END = 1U << 2,
    KBX_VOICE_INTERRUPTED = 1U << 3,
    KBX_VOICE_SESSION_END = 1U << 4,
    KBX_VOICE_OUTPUT_SEEN = 1U << 5,
};

static inline unsigned kbx_voice_phase(const cJSON *event)
{
    if (!cJSON_IsString(event)) return 0;
    const char *name = event->valuestring;
    if (!strcmp(name, "input_partial")) return KBX_VOICE_INPUT_HEARD;
    if (!strcmp(name, "input_final")) return KBX_VOICE_INPUT_HEARD | KBX_VOICE_INPUT_FINAL;
    if (!strcmp(name, "turn_end")) return KBX_VOICE_TURN_END;
    if (!strcmp(name, "interrupted")) return KBX_VOICE_INTERRUPTED;
    if (!strcmp(name, "session_end")) return KBX_VOICE_SESSION_END;
    if (!strcmp(name, "output_text") || !strcmp(name, "output_audio")) return KBX_VOICE_OUTPUT_SEEN;
    return 0;
}

/* SpeechKit attaches event metadata to transcripts as well as standalone
 * event frames; event_types can carry several phases in the same message. */
static inline unsigned kbx_voice_phases(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    unsigned phases = kbx_voice_phase(type) |
        kbx_voice_phase(cJSON_GetObjectItemCaseSensitive(root, "event_type"));
    const cJSON *types = cJSON_GetObjectItemCaseSensitive(root, "event_types");
    if (cJSON_IsArray(types)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, types) phases |= kbx_voice_phase(item);
    }
    if (cJSON_IsString(type) && !strcmp(type->valuestring, "input_transcript")) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) phases |= KBX_VOICE_INPUT_HEARD;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "done")))
            phases |= KBX_VOICE_INPUT_HEARD | KBX_VOICE_INPUT_FINAL;
    }
    if (cJSON_IsString(type) && !strcmp(type->valuestring, "output_transcript"))
        phases |= KBX_VOICE_OUTPUT_SEEN;
    /* Older SpeechKit Deepgram adapters expose Flux input boundaries as
     * turn_end too. Only AgentAudioDone finishes the assistant's audio. */
    const cJSON *metadata = cJSON_GetObjectItemCaseSensitive(root, "provider_metadata");
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(metadata, "provider_event");
    if (cJSON_IsString(event)) {
        if (!strcmp(event->valuestring, "EagerEndOfTurn")) {
            phases = (phases & ~(unsigned)KBX_VOICE_TURN_END) | KBX_VOICE_INPUT_HEARD;
        } else if (!strcmp(event->valuestring, "EndOfTurn")) {
            phases = (phases & ~(unsigned)KBX_VOICE_TURN_END) |
                KBX_VOICE_INPUT_HEARD | KBX_VOICE_INPUT_FINAL;
        } else if (!strcmp(event->valuestring, "AgentAudioDone")) {
            phases |= KBX_VOICE_OUTPUT_SEEN | KBX_VOICE_TURN_END;
        } else if (!strcmp(event->valuestring, "StartOfTurn") ||
                   !strcmp(event->valuestring, "TurnResumed") ||
                   !strcmp(event->valuestring, "UserStartedSpeaking")) {
            phases |= KBX_VOICE_INPUT_HEARD;
        }
    }
    return phases;
}

static inline bool kbx_voice_turn_ended(unsigned phases, bool output_seen)
{
    return (phases & KBX_VOICE_TURN_END) &&
        (output_seen || (phases & KBX_VOICE_OUTPUT_SEEN));
}
