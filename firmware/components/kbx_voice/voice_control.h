#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
