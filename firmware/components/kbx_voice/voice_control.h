#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

#define KBX_VOICE_CONTROL_ID_MAX 128U
#define KBX_VOICE_ECHO_GUARD_US INT64_C(350000)
#define KBX_VOICE_ONESHOT_DONE_US INT64_C(2000000)

typedef enum {
    KBX_VOICE_REPLY_WAIT = 0,
    KBX_VOICE_REPLY_CLOSE,
    KBX_VOICE_REPLY_LISTEN,
} kbx_voice_reply_action_t;

/* Called only after qualified reply EOF and local playback drain. A mode
 * request in flight must survive the ordinary one-shot completion timer. */
static inline kbx_voice_reply_action_t kbx_voice_after_reply(bool conversation,
    bool pending, bool capture_running, int64_t quiet_us)
{
    if (pending) return KBX_VOICE_REPLY_WAIT;
    if (!conversation) return quiet_us >= KBX_VOICE_ONESHOT_DONE_US ?
        KBX_VOICE_REPLY_CLOSE : KBX_VOICE_REPLY_WAIT;
    return !capture_running && quiet_us >= KBX_VOICE_ECHO_GUARD_US ?
        KBX_VOICE_REPLY_LISTEN : KBX_VOICE_REPLY_WAIT;
}

typedef enum {
    KBX_VOICE_MODE_ONESHOT = 0,
    KBX_VOICE_MODE_CONVERSATION,
} kbx_voice_mode_t;

typedef struct {
    bool accepted;
    kbx_voice_mode_t mode;
    char conversation_session_id[KBX_VOICE_CONTROL_ID_MAX + 1U];
    char request_id[KBX_VOICE_CONTROL_ID_MAX + 1U];
} kbx_voice_control_t;

static inline bool kbx_voice_control_id(const cJSON *value)
{
    if (!cJSON_IsString(value) || !value->valuestring[0]) return false;
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)value->valuestring; *p; ++p) {
        if (++length > KBX_VOICE_CONTROL_ID_MAX ||
            !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr("_.:-", *p))) return false;
    }
    return true;
}

/* Only the authenticated server's typed receipt for THIS voice session can
 * change capture mode. Neither spoken text nor assistant output is control. */
static inline bool kbx_voice_control(const cJSON *root, const char *voice_session_id,
                                    kbx_voice_control_t *out)
{
    memset(out, 0, sizeof(*out));
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event_type");
    if (!voice_session_id || !voice_session_id[0] || !cJSON_IsString(type) ||
        strcmp(type->valuestring, "event") || !cJSON_IsString(event) ||
        strcmp(event->valuestring, "conversation_mode")) return false;
    const cJSON *receipt = cJSON_GetObjectItemCaseSensitive(root, "conversation");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(receipt, "version");
    const cJSON *voice = cJSON_GetObjectItemCaseSensitive(receipt, "voice_session_id");
    const cJSON *history = cJSON_GetObjectItemCaseSensitive(receipt, "conversation_session_id");
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(receipt, "request_id");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(receipt, "mode");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(receipt, "status");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1.0 ||
        !kbx_voice_control_id(voice) || strcmp(voice->valuestring, voice_session_id) ||
        !kbx_voice_control_id(history) || !kbx_voice_control_id(request) ||
        !cJSON_IsString(mode) || !cJSON_IsString(status)) return false;
    if (!strcmp(mode->valuestring, "oneshot")) out->mode = KBX_VOICE_MODE_ONESHOT;
    else if (!strcmp(mode->valuestring, "conversation")) out->mode = KBX_VOICE_MODE_CONVERSATION;
    else return false;
    if (!strcmp(status->valuestring, "accepted")) out->accepted = true;
    else if (strcmp(status->valuestring, "rejected")) return false;
    memcpy(out->conversation_session_id, history->valuestring, strlen(history->valuestring) + 1U);
    memcpy(out->request_id, request->valuestring, strlen(request->valuestring) + 1U);
    return true;
}

/* Front door (SpeechKit "Device front door"): on a plain session that sent
 * front_door routes ["agent"], the server answers a spoken Companion request
 * (or a model hand-over) with an event frame route_switch. The device closes
 * the session, serves THIS turn on the Companion route and replays the
 * utterance as the first text frame. Like the mode receipt it is typed and
 * bound to this voice session; spoken or generated text is never a switch. */
#define KBX_VOICE_UTTERANCE_MAX 512U
#define KBX_VOICE_REASON_MAX 24U

typedef struct {
    char request_id[KBX_VOICE_CONTROL_ID_MAX + 1U];
    char reason[KBX_VOICE_REASON_MAX + 1U];
    char utterance[KBX_VOICE_UTTERANCE_MAX + 1U];
} kbx_voice_route_switch_t;

/* Strict UTF-8 (no overlongs, surrogates or code points above U+10FFFF), so a
 * replayed utterance always stays valid inside a JSON frame. */
static inline bool kbx_voice_utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p++;
        if (c < 0x80U) continue;
        unsigned extra;
        unsigned min;
        unsigned cp;
        if (c >= 0xC2U && c <= 0xDFU) { extra = 1; min = 0x80U; cp = c & 0x1FU; }
        else if (c >= 0xE0U && c <= 0xEFU) { extra = 2; min = 0x800U; cp = c & 0x0FU; }
        else if (c >= 0xF0U && c <= 0xF4U) { extra = 3; min = 0x10000U; cp = c & 0x07U; }
        else return false;
        while (extra--) {
            if ((*p & 0xC0U) != 0x80U) return false;
            cp = (cp << 6) | (*p++ & 0x3FU);
        }
        if (cp < min || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) return false;
    }
    return true;
}

static inline bool kbx_voice_route_switch(const cJSON *root, const char *voice_session_id,
                                          kbx_voice_route_switch_t *out)
{
    memset(out, 0, sizeof(*out));
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event_type");
    if (!voice_session_id || !voice_session_id[0] || !cJSON_IsString(type) ||
        strcmp(type->valuestring, "event") || !cJSON_IsString(event) ||
        strcmp(event->valuestring, "route_switch")) return false;
    const cJSON *route = cJSON_GetObjectItemCaseSensitive(root, "route");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(route, "version");
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(route, "to");
    const cJSON *voice = cJSON_GetObjectItemCaseSensitive(route, "voice_session_id");
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(route, "request_id");
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(route, "reason");
    const cJSON *utterance = cJSON_GetObjectItemCaseSensitive(route, "utterance");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1.0 ||
        !cJSON_IsString(to) || strcmp(to->valuestring, "agent") ||
        !kbx_voice_control_id(voice) || strcmp(voice->valuestring, voice_session_id) ||
        !kbx_voice_control_id(request) ||
        !cJSON_IsString(reason) || (strcmp(reason->valuestring, "explicit_companion") &&
                                    strcmp(reason->valuestring, "model_handoff")) ||
        !cJSON_IsString(utterance) || !utterance->valuestring[0] ||
        strlen(utterance->valuestring) > KBX_VOICE_UTTERANCE_MAX ||
        !kbx_voice_utf8_valid(utterance->valuestring)) return false;
    memcpy(out->request_id, request->valuestring, strlen(request->valuestring) + 1U);
    memcpy(out->reason, reason->valuestring, strlen(reason->valuestring) + 1U);
    memcpy(out->utterance, utterance->valuestring, strlen(utterance->valuestring) + 1U);
    return true;
}

/* {"type":"text","text":"<text>"} with the JSON escapes a transcript can need
 * (quote, backslash, control characters). False when it does not fit. */
static inline bool kbx_voice_text_frame(const char *text, char *out, size_t capacity)
{
    static const char head[] = "{\"type\":\"text\",\"text\":\"";
    size_t used = sizeof(head) - 1U;
    if (!text || !out || capacity < used + 3U) return false;
    memcpy(out, head, used);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char esc[7];
        size_t n;
        if (*p == '"' || *p == '\\') { esc[0] = '\\'; esc[1] = (char)*p; n = 2; }
        else if (*p < 0x20U) { n = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", *p); }
        else { esc[0] = (char)*p; n = 1; }
        if (used + n + 3U > capacity) return false;
        memcpy(out + used, esc, n);
        used += n;
    }
    out[used++] = '"';
    out[used++] = '}';
    out[used] = '\0';
    return true;
}
