#pragma once

/* User-facing voice failure classes. Every failed tap ends in exactly one of
 * these so the display can say why, and offer the matching next step.
 * Pure (cJSON only) so the native test covers both server error shapes:
 *   SpeechKit: {"error":{"code":"...","message":"..."}}
 *   Gateway:   {"error":"speechkit_preflight_denied","error_code":"...",
 *               "reason_code":"...","request_id":"..."}
 *   OAuth:     {"error":"invalid_grant"}
 * Credentials never enter these structs; only server codes and ids. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

typedef enum {
    KBX_VOICE_ERR_NONE = 0,
    KBX_VOICE_ERR_OFFLINE,  /* no Wi-Fi / no route / connect failed */
    KBX_VOICE_ERR_SIGNIN,   /* account credential rejected: pair again */
    KBX_VOICE_ERR_SERVICE,  /* server or provider unavailable, timeouts */
    KBX_VOICE_ERR_DENIED,   /* not entitled, budget, quota, rate limit */
    KBX_VOICE_ERR_UNKNOWN,  /* anything else; display shows the code */
} kbx_voice_error_kind_t;

#define KBX_VOICE_ERROR_CODE_MAX 64U
#define KBX_VOICE_ERROR_SHORT_MAX 40U

typedef struct {
    kbx_voice_error_kind_t kind;
    int http_status; /* 0 = no HTTP status (transport, in-band, local) */
    char code[KBX_VOICE_ERROR_CODE_MAX];
    char reason[KBX_VOICE_ERROR_CODE_MAX];
    char request_id[KBX_VOICE_ERROR_CODE_MAX];
} kbx_voice_error_t;

static inline bool kbx_voice_error_has(const char *s, const char *needle)
{
    return s && needle && strstr(s, needle) != NULL;
}

static inline bool kbx_voice_error_any(const char *code, const char *reason, const char *needle)
{
    return kbx_voice_error_has(code, needle) || kbx_voice_error_has(reason, needle);
}

/* status <= 0 means the code came without an HTTP status (WS in-band). */
static inline kbx_voice_error_kind_t kbx_voice_error_classify(int status,
    const char *code, const char *reason)
{
    if (status == 401 || kbx_voice_error_any(code, reason, "token_expired") ||
        kbx_voice_error_any(code, reason, "auth_expired") ||
        kbx_voice_error_any(code, reason, "invalid_grant") ||
        kbx_voice_error_any(code, reason, "unauthenticated") ||
        kbx_voice_error_any(code, reason, "audience_mismatch")) {
        return KBX_VOICE_ERR_SIGNIN;
    }
    if (status == 402 || status == 429) return KBX_VOICE_ERR_DENIED;
    if (status >= 500 || status == 408 ||
        kbx_voice_error_any(code, reason, "unavailable") ||
        kbx_voice_error_any(code, reason, "timeout") ||
        kbx_voice_error_any(code, reason, "connect_failed") ||
        kbx_voice_error_any(code, reason, "global_limit")) {
        return KBX_VOICE_ERR_SERVICE;
    }
    if (kbx_voice_error_any(code, reason, "budget") ||
        kbx_voice_error_any(code, reason, "quota") ||
        kbx_voice_error_any(code, reason, "entitle") ||
        kbx_voice_error_any(code, reason, "consent") ||
        kbx_voice_error_any(code, reason, "disabled") ||
        kbx_voice_error_any(code, reason, "denied") ||
        kbx_voice_error_any(code, reason, "rate_limit") ||
        kbx_voice_error_any(code, reason, "limit_exceeded") ||
        kbx_voice_error_any(code, reason, "not_allowed") ||
        kbx_voice_error_any(code, reason, "owner_mismatch")) {
        return KBX_VOICE_ERR_DENIED;
    }
    /* A 403 that names an invalid request is a contract fault, not a
     * permission the owner can fix; show it with its code instead. */
    if (status == 403 && !kbx_voice_error_any(code, reason, "invalid"))
        return KBX_VOICE_ERR_DENIED;
    return KBX_VOICE_ERR_UNKNOWN;
}

static inline void kbx_voice_error_copy(char *out, const cJSON *value)
{
    if (cJSON_IsString(value) && value->valuestring && value->valuestring[0] && !out[0]) {
        snprintf(out, KBX_VOICE_ERROR_CODE_MAX, "%s", value->valuestring);
    }
}

/* Non-2xx HTTP response -> classified error. body may be NULL or not JSON. */
static inline void kbx_voice_error_from_http(int status, const char *body, kbx_voice_error_t *out)
{
    memset(out, 0, sizeof(*out));
    out->http_status = status;
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    if (cJSON_IsObject(root)) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        /* Most specific first: flat error_code, nested error.code, then the
         * flat error string (Gateway family name or OAuth error). */
        kbx_voice_error_copy(out->code, cJSON_GetObjectItemCaseSensitive(root, "error_code"));
        if (cJSON_IsObject(error)) {
            kbx_voice_error_copy(out->code, cJSON_GetObjectItemCaseSensitive(error, "code"));
            kbx_voice_error_copy(out->reason, cJSON_GetObjectItemCaseSensitive(error, "reason_code"));
            kbx_voice_error_copy(out->request_id, cJSON_GetObjectItemCaseSensitive(error, "request_id"));
        }
        kbx_voice_error_copy(out->reason, cJSON_GetObjectItemCaseSensitive(root, "reason_code"));
        kbx_voice_error_copy(out->code, error);
        kbx_voice_error_copy(out->request_id, cJSON_GetObjectItemCaseSensitive(root, "request_id"));
    }
    cJSON_Delete(root);
    if (!out->code[0]) snprintf(out->code, sizeof(out->code), "http_%d", status);
    out->kind = kbx_voice_error_classify(status, out->code, out->reason);
}

/* Local or in-band failure with a known class or a server WS error code. */
static inline void kbx_voice_error_set(kbx_voice_error_t *out, kbx_voice_error_kind_t kind,
    int status, const char *code)
{
    memset(out, 0, sizeof(*out));
    out->http_status = status;
    snprintf(out->code, sizeof(out->code), "%s", code && code[0] ? code : "unknown");
    out->kind = kind != KBX_VOICE_ERR_NONE ? kind : kbx_voice_error_classify(status, out->code, NULL);
}

/* Short display code: "<status> <code>" without the speechkit_ prefix. */
static inline void kbx_voice_error_short(const kbx_voice_error_t *e, char *out, size_t capacity)
{
    if (!capacity) return;
    const char *code = e->code;
    if (!strncmp(code, "speechkit_", 10)) code += 10;
    size_t used = 0;
    out[0] = '\0';
    if (e->http_status > 0) {
        int n = snprintf(out, capacity, "%d ", e->http_status);
        used = n < 0 ? 0 : ((size_t)n < capacity ? (size_t)n : capacity - 1U);
    }
    /* Deliberate truncation: the display line is short. */
    while (used + 1U < capacity && *code) out[used++] = *code++;
    out[used] = '\0';
}

static inline const char *kbx_voice_error_name(kbx_voice_error_kind_t kind)
{
    switch (kind) {
    case KBX_VOICE_ERR_OFFLINE: return "offline";
    case KBX_VOICE_ERR_SIGNIN: return "signin";
    case KBX_VOICE_ERR_SERVICE: return "service";
    case KBX_VOICE_ERR_DENIED: return "denied";
    case KBX_VOICE_ERR_UNKNOWN: return "unknown";
    case KBX_VOICE_ERR_NONE:
    default: return "none";
    }
}
