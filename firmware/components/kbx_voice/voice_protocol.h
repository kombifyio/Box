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

/* ── Tool states (Voice Agent with tools) ───────────────────────────────────
 * The server describes a tool turn with four frames: tool_call names the
 * tool that starts, tool_result_ack closes it, sequence_step reports the
 * steps of a multi-step tool, and tool_bridge_unavailable says the bridge
 * that would run the tool is not there. The Box shows the tool's display
 * name while the tool runs; the bridge frame must clear that state instead
 * of leaving a stale "working on it" on the display. */
typedef enum {
    KBX_VOICE_TOOL_NONE = 0,
    KBX_VOICE_TOOL_CALL,
    KBX_VOICE_TOOL_ACK,
    KBX_VOICE_TOOL_STEP,
    KBX_VOICE_TOOL_UNAVAILABLE,
} kbx_voice_tool_state_t;

#define KBX_VOICE_TOOL_NAME_MAX 40U
#define KBX_VOICE_TOOL_ID_MAX 128U
#define KBX_VOICE_TOOL_STATUS_MAX 24U

typedef struct {
    kbx_voice_tool_state_t state;
    char name[KBX_VOICE_TOOL_NAME_MAX];
    char step_id[KBX_VOICE_TOOL_ID_MAX + 1U];
    char status[KBX_VOICE_TOOL_STATUS_MAX + 1U];
} kbx_voice_tool_t;

/* The tool's display name: printable ASCII only, so it can never carry a
 * frame delimiter or a control sequence into the display or the log. */
static inline void kbx_voice_tool_name(const cJSON *root, char *out, size_t capacity)
{
    if (!out || !capacity) return;
    out[0] = '\0';
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name) || !name->valuestring[0]) return;
    size_t n = 0;
    for (; name->valuestring[n] && n + 1U < capacity; ++n) {
        const unsigned char c = (unsigned char)name->valuestring[n];
        out[n] = (c >= 32U && c < 127U) ? (char)c : '?';
    }
    out[n] = '\0';
}

/* Classify a server frame as a tool state. The frame's "type" carries the
 * event; SpeechKit also repeats it as "event_type". A frame that is not one
 * of the four tool frames is not a tool state. */
static inline kbx_voice_tool_state_t kbx_voice_tool_state(const cJSON *root, kbx_voice_tool_t *out)
{
    if (!out) return KBX_VOICE_TOOL_NONE;
    memset(out, 0, sizeof(*out));
    if (!root || !cJSON_IsObject(root)) return KBX_VOICE_TOOL_NONE;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event_type");
    const char *name = cJSON_IsString(type) ? type->valuestring : NULL;
    const char *ev = cJSON_IsString(event) ? event->valuestring : NULL;
    kbx_voice_tool_state_t state = KBX_VOICE_TOOL_NONE;
    if ((name && !strcmp(name, "tool_call")) || (ev && !strcmp(ev, "tool_call"))) {
        state = KBX_VOICE_TOOL_CALL;
    } else if (ev && !strcmp(ev, "tool_result_ack")) {
        state = KBX_VOICE_TOOL_ACK;
    } else if (name && !strcmp(name, "sequence_step")) {
        state = KBX_VOICE_TOOL_STEP;
    } else if ((name && !strcmp(name, "tool_bridge_unavailable")) ||
               (ev && !strcmp(ev, "tool_bridge_unavailable"))) {
        state = KBX_VOICE_TOOL_UNAVAILABLE;
    }
    if (state == KBX_VOICE_TOOL_NONE) return KBX_VOICE_TOOL_NONE;
    out->state = state;
    if (state == KBX_VOICE_TOOL_CALL) kbx_voice_tool_name(root, out->name, sizeof(out->name));
    if (state == KBX_VOICE_TOOL_STEP) {
        const cJSON *step = cJSON_GetObjectItemCaseSensitive(root, "step_id");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        if (cJSON_IsString(step) && step->valuestring[0] &&
            strlen(step->valuestring) <= KBX_VOICE_TOOL_ID_MAX) {
            memcpy(out->step_id, step->valuestring, strlen(step->valuestring) + 1U);
        }
        if (cJSON_IsString(status) && status->valuestring[0] &&
            strlen(status->valuestring) <= KBX_VOICE_TOOL_STATUS_MAX) {
            memcpy(out->status, status->valuestring, strlen(status->valuestring) + 1U);
        }
    }
    return state;
}

/* ── Voice route (fast | agent) ───────────────────────────────────────────
 * The route is the owner's setting for the turn: "fast" answers on the
 * plain voice agent of the account, "agent" answers on the account-owned
 * Companion. It is stored as a name, never as a number, so a future route
 * can be added without moving the old values. An unknown or missing name
 * is the default route; the Companion is the current default because the
 * account owns the routing (the plain route stays the fallback). */
typedef enum {
    KBX_VOICE_ROUTE_AGENT = 0,
    KBX_VOICE_ROUTE_FAST = 1,
} kbx_voice_route_t;

#define KBX_VOICE_ROUTE_NAME_MAX 16U

static inline bool kbx_voice_route_parse(const char *name, kbx_voice_route_t *out)
{
    if (!name || !out) return false;
    if (!strcmp(name, "agent")) { *out = KBX_VOICE_ROUTE_AGENT; return true; }
    if (!strcmp(name, "fast")) { *out = KBX_VOICE_ROUTE_FAST; return true; }
    return false;
}

static inline const char *kbx_voice_route_name(kbx_voice_route_t route)
{
    return route == KBX_VOICE_ROUTE_FAST ? "fast" : "agent";
}

/* ── Session renewal on an expired authorization ─────────────────────────
 * The server closes the socket with the reason "authorization_expired" when
 * the session's authorization ran out; the answer is not lost, the session
 * is renewed. Any other close reason ends the turn as before. */
#define KBX_VOICE_CLOSE_AUTHORIZATION_EXPIRED "authorization_expired"

static inline bool kbx_voice_close_renews(const char *reason)
{
    return reason && !strcmp(reason, KBX_VOICE_CLOSE_AUTHORIZATION_EXPIRED);
}
