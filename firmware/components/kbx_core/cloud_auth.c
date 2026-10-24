/* Standalone public-client RFC 8628 login. Refresh tokens never leave NVS/TLS.
 * A generation fences every HTTP result against forget and a newer pairing. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "cJSON.h"

/* Parse through the existing cJSON allocator (installed in app_main with
 * PSRAM hooks). Reject duplicate keys, missing fields and truncation. */
static bool json_field(const char *json, const char *name, char *out, size_t cap, int *number)
{
    if (strstr(json, "\\u0000")) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &end, true);
    if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    const cJSON *match = NULL;
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (item->string && !strcmp(item->string, name)) {
            if (match) { cJSON_Delete(root); return false; }
            match = item;
        }
    }
    bool ok = false;
    if (number) {
        if (cJSON_IsNumber(match) && match->valuedouble >= 0 &&
            match->valuedouble <= INT_MAX && (double)match->valueint == match->valuedouble) {
            *number = match->valueint; ok = true;
        }
    } else if (cJSON_IsString(match) && match->valuestring && cap && strlen(match->valuestring) < cap) {
        bool safe = true;
        for (const unsigned char *p = (const unsigned char *)match->valuestring; *p; ++p) {
            if (*p < 32 || *p > 126) { safe = false; break; }
        }
        if (safe) { memcpy(out, match->valuestring, strlen(match->valuestring) + 1); ok = true; }
    }
    cJSON_Delete(root);
    return ok;
}
static bool json_has_field(const char *json, const char *name)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    bool found = root && cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, name);
    cJSON_Delete(root);
    return found;
}

static const char *access_response_error(const char *json, char *access, size_t capacity,
                                        int *seconds, size_t *access_bytes)
{
    *seconds = 0; *access_bytes = 0;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return "invalid_oauth_json"; }
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "access_token");
    const char *error = NULL;
    if (!token) error = "missing_access_token";
    else if (!cJSON_IsString(token) || !token->valuestring) error = "invalid_access_token_type";
    else {
        *access_bytes = strlen(token->valuestring);
        if (!*access_bytes) error = "empty_access_token";
        else if (*access_bytes >= capacity) error = "access_token_too_large";
    }
    cJSON_Delete(root);
    if (error) return error;
    if (!json_field(json, "access_token", access, capacity, NULL)) return "invalid_access_token";
    if (!json_has_field(json, "expires_in")) return "missing_token_expiry";
    if (!json_field(json, "expires_in", NULL, 0, seconds) || *seconds <= 0) return "invalid_token_expiry";
    return NULL;
}
static bool form_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in; ++in) {
        unsigned char c = (unsigned char)*in;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        size_t bytes = safe ? 1u : 3u;
        if (n + bytes >= cap) return false;
        if (safe) out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = 0;
    return true;
}

/* Return literals only: an upstream error field is untrusted and must never
 * become an arbitrary log/status channel for credentials or response text. */
static const char *safe_oauth_error(const char *code)
{
    static const char *const allowed[] = {
        "authorization_pending", "slow_down", "access_denied", "expired_token",
        "invalid_grant", "invalid_client", "unauthorized_client", "invalid_request",
        "unsupported_grant_type", "invalid_scope", "server_error",
        "temporarily_unavailable", "invalid_token", "unsupported_token_type",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (!strcmp(code, allowed[i])) return allowed[i];
    }
    return "unknown_oauth_error";
}

static bool poll_http_retryable(int status)
{
    return status == 429 || (status >= 500 && status <= 599);
}

static int poll_backoff_interval(int interval)
{
    if (interval < 5) interval = 5;
    return interval < 1800 ? interval * 2 : 3600;
}

static bool base64url_payload(const char *in, size_t len, char *out, size_t cap)
{
    if (!len || len % 4 == 1 || !cap) return false;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)in[i];
        unsigned value = c >= 'A' && c <= 'Z' ? (unsigned)(c - 'A') :
            c >= 'a' && c <= 'z' ? (unsigned)(c - 'a') + 26u :
            c >= '0' && c <= '9' ? (unsigned)(c - '0') + 52u :
            c == '-' ? 62u : c == '_' ? 63u : 64u;
        if (value == 64u) return false;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            unsigned char decoded = (unsigned char)(accumulator >> bits);
            if (!decoded || n + 1 >= cap) return false;
            out[n++] = (char)decoded;
        }
    }
    if (bits && (accumulator & ((1u << bits) - 1u))) return false;
    out[n] = 0;
    return true;
}

/* The pinned HTTPS issuer authenticates this response. Gateway still verifies
 * JWT signatures. Reject even a successful token response if it targets a
 * different API, issuer or expired lease; never forward such a credential. */
static bool jwt_claims_valid(const char *token, int64_t now, char *scratch, size_t cap, int *remaining)
{
    if (now < 1704067200LL) return false; /* SNTP must have established time. */
    const char *first = strchr(token, '.');
    const char *second = first ? strchr(first + 1, '.') : NULL;
    if (!first || first == token || !second || !second[1] || strchr(second + 1, '.') ||
        !base64url_payload(first + 1, (size_t)(second - first - 1), scratch, cap)) return false;
    if (strstr(scratch, "\\u0000")) return false;
    cJSON *root = cJSON_ParseWithOpts(scratch, NULL, true);
    if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    const cJSON *aud = NULL, *exp = NULL, *iss = NULL, *item;
    bool valid = true;
    cJSON_ArrayForEach(item, root) {
        const cJSON **field = item->string && !strcmp(item->string, "aud") ? &aud :
            item->string && !strcmp(item->string, "exp") ? &exp :
            item->string && !strcmp(item->string, "iss") ? &iss : NULL;
        if (field) { if (*field) valid = false; *field = item; }
    }
    bool audience = cJSON_IsString(aud) && !strcmp(aud->valuestring, "https://api.kombify.io");
    if (cJSON_IsArray(aud)) {
        cJSON_ArrayForEach(item, aud) {
            if (!cJSON_IsString(item)) valid = false;
            else if (!strcmp(item->valuestring, "https://api.kombify.io")) audience = true;
        }
    }
    valid = valid && audience && cJSON_IsString(iss) &&
        !strcmp(iss->valuestring, "https://login.kombify.io/") && cJSON_IsNumber(exp);
    if (valid) {
        double expiry = exp->valuedouble;
        valid = expiry > (double)now && expiry <= 253402300799.0 &&
            (double)(int64_t)expiry == expiry;
        if (valid) {
            int64_t lease = (int64_t)expiry - now;
            /* A tenant may legitimately issue a token lasting several days.
             * Bound our cached lease, not the issuer's accepted JWT lifetime. */
            *remaining = (int)(lease < 86400 ? lease : 86400);
        }
    }
    cJSON_Delete(root);
    return valid;
}

#ifndef CLOUD_AUTH_NATIVE_TEST
#include <time.h>
#include "cloud_auth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

/* Boards without PSRAM (the ESP32-C6 variant) keep the work buffers and the
 * pairing task stack in internal RAM, so they get a 16 KiB token response
 * bound and a 1x encode buffer: it still holds a decoded JWT payload and a
 * form-encoded refresh token of normal Auth0 size, and the work area drops
 * from ~82 KiB to ~51 KiB so the TLS handshake that follows keeps room. */
#if CONFIG_SPIRAM
#define AUTH_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AUTH_RESP_CAP 32768
#define AUTH_ENCODED_CAP (CLOUD_AUTH_TOKEN_MAX * 3)
#else
#define AUTH_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define AUTH_RESP_CAP 16384
#define AUTH_ENCODED_CAP CLOUD_AUTH_TOKEN_MAX
#endif

/* Renew the access token once less than this is left. A board whose
 * token is used only to mint a session may pass a shorter margin. */
#ifndef CLOUD_AUTH_RENEW_MARGIN_S
#define CLOUD_AUTH_RENEW_MARGIN_S 600
#endif

#define CLIENT_ID "hnovzaA6thagsgOYb5P2HdJp3mzg6iNg"
#define AUTH_ROOT "https://login.kombify.io"
#define TOKEN_URL AUTH_ROOT "/oauth/token"
#define CODE_URL AUTH_ROOT "/oauth/device/code"
#define RESP_CAP AUTH_RESP_CAP
#define FORM_CAP (AUTH_ENCODED_CAP + 512)
#define DEVICE_CAP 2048

static const char *TAG = "cloud_auth";
static SemaphoreHandle_t s_lock, s_http_lock;
typedef struct {
    char access[CLOUD_AUTH_TOKEN_MAX];
    char refresh[CLOUD_AUTH_TOKEN_MAX];
    cloud_auth_status_t status;
    uint32_t generation;
    int64_t deadline_us;
    int64_t pairing_deadline_us;
    bool worker_running;
} auth_state_t;
static auth_state_t *s;
typedef struct {
    char response[RESP_CAP];
    char form[FORM_CAP];
    char encoded[AUTH_ENCODED_CAP];
    char access[CLOUD_AUTH_TOKEN_MAX];
    char refresh[CLOUD_AUTH_TOKEN_MAX];
    char device[DEVICE_CAP];
} auth_work_t;

static void wipe(void *p, size_t n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }
/* Optional caller-owned work area (cloud_auth_set_work_area): a board
 * without PSRAM lends a static buffer while it is idle, so a renewal does
 * not need one large block from a fragmented heap. */
static void *s_area;
static size_t s_area_size;
static bool s_area_busy;

static auth_work_t *work_new(void)
{
    if (s_area && s_area_size >= sizeof(auth_work_t) && !s_area_busy) {
        s_area_busy = true;
        memset(s_area, 0, sizeof(auth_work_t));
        return (auth_work_t *)s_area;
    }
    return heap_caps_calloc(1, sizeof(auth_work_t), AUTH_CAPS);
}

static void work_free(auth_work_t *w)
{
    if (!w) return;
    wipe(w, sizeof(*w));
    if ((void *)w == s_area) s_area_busy = false;
    else free(w);
}

bool cloud_auth_set_work_area(void *area, size_t size)
{
    if (s_area_busy) return false;
    s_area = area;
    s_area_size = area ? size : 0;
    return true;
}

size_t cloud_auth_work_size(void)
{
    return sizeof(auth_work_t);
}
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static bool current(uint32_t generation) { lock(); bool ok = s->generation == generation; unlock(); return ok; }
static void error_set(uint32_t generation, const char *code)
{
    lock(); if (s->generation == generation) strlcpy(s->status.error_code, code, sizeof(s->status.error_code)); unlock();
}
static esp_err_t save_refresh(const char *refresh)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("cloud_auth", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = refresh[0] ? nvs_set_str(h, "refresh", refresh) : nvs_erase_key(h, "refresh");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Redirects are forbidden: OAuth credentials only reach this fixed authority. */
static esp_err_t post(auth_work_t *w, const char *url, int *status)
{
    wipe(w->response, sizeof(w->response));
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true, .buffer_size = 1024, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_header(client, "Accept", "application/json");
    size_t len = strlen(w->form);
    esp_err_t err = esp_http_client_open(client, (int)len);
    if (err == ESP_OK) {
        size_t sent = 0;
        while (sent < len) {
            int n = esp_http_client_write(client, w->form + sent, (int)(len - sent));
            if (n <= 0) { err = ESP_FAIL; break; }
            sent += (size_t)n;
        }
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(client);
        size_t used = 0;
        for (;;) {
            int n = esp_http_client_read(client, w->response + used, (int)(sizeof(w->response) - 1 - used));
            if (n < 0) { err = ESP_FAIL; break; }
            if (n == 0) { if (!esp_http_client_is_complete_data_received(client)) err = ESP_FAIL; break; }
            used += (size_t)n;
            if (used == sizeof(w->response) - 1) { err = ESP_ERR_INVALID_SIZE; break; }
        }
        w->response[used] = 0;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

/* Called under HTTP lock; state lock covers NVS commit and generation fence.
 * Persistence precedes publication so an uncommitted rotated token is never
 * advertised as a durable successful login. */
static esp_err_t accept_tokens(auth_work_t *w, uint32_t generation, bool refreshing)
{
    int seconds = 0;
    size_t access_bytes = 0;
    wipe(w->access, sizeof(w->access)); wipe(w->refresh, sizeof(w->refresh));
    const char *response_error = access_response_error(w->response, w->access, sizeof(w->access), &seconds, &access_bytes);
    if (response_error) {
        ESP_LOGW(TAG, "token rejected http_status=200 error_code=%s access_bytes=%u expires_in=%d",
                 response_error, (unsigned)access_bytes, seconds);
        error_set(generation, response_error); return ESP_FAIL;
    }
    int jwt_seconds = 0;
    if (!jwt_claims_valid(w->access, (int64_t)time(NULL), w->encoded, sizeof(w->encoded), &jwt_seconds)) {
        ESP_LOGW(TAG, "token rejected http_status=200 error_code=token_claims_invalid access_bytes=%u expires_in=%d",
                 (unsigned)access_bytes, seconds);
        error_set(generation, "token_claims_invalid"); return ESP_FAIL;
    }
    if (jwt_seconds < seconds) seconds = jwt_seconds;
    /* An omitted refresh token on rotation retains the previous credential.
     * A present-but-invalid value must not silently take that fallback. */
    bool has_refresh = json_field(w->response, "refresh_token", w->refresh, sizeof(w->refresh), NULL);
    if (!has_refresh && json_has_field(w->response, "refresh_token")) { error_set(generation, "invalid_refresh_response"); return ESP_FAIL; }
    lock();
    if (s->generation != generation) { unlock(); return ESP_ERR_INVALID_STATE; }
    const char *refresh = has_refresh ? w->refresh : refreshing ? s->refresh : "";
    if (!refresh[0]) { strlcpy(s->status.error_code, "refresh_token_missing", sizeof(s->status.error_code)); unlock(); return ESP_FAIL; }
    esp_err_t err = save_refresh(refresh);
    if (err == ESP_OK) {
        if (has_refresh) strlcpy(s->refresh, refresh, sizeof(s->refresh));
        strlcpy(s->access, w->access, sizeof(s->access));
        s->deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
        s->status.paired = true; s->status.pairing = false;
        s->status.user_code[0] = 0; s->status.verification_uri[0] = 0;
        s->status.verification_uri_complete[0] = 0;
        s->status.error_code[0] = 0;
    } else {
        /* The server may already have rotated. Never keep using cached access
         * after a durability failure; the operator must pair again. */
        wipe(s->access, sizeof(s->access)); wipe(s->refresh, sizeof(s->refresh));
        s->deadline_us = 0; s->status.paired = false;
        strlcpy(s->status.error_code, "refresh_persist_failed", sizeof(s->status.error_code));
    }
    unlock();
    return err;
}

static void invalidate(uint32_t generation)
{
    lock();
    if (s->generation == generation) {
        ++s->generation; wipe(s->access, sizeof(s->access)); wipe(s->refresh, sizeof(s->refresh));
        s->deadline_us = 0; s->status.paired = false; s->status.pairing = false;
        s->status.user_code[0] = 0; s->status.verification_uri[0] = 0;
        s->status.verification_uri_complete[0] = 0;
        esp_err_t err = save_refresh("");
        strlcpy(s->status.error_code, err == ESP_OK ? "pairing_required" : "forget_persist_failed", sizeof(s->status.error_code));
    }
    unlock();
}

static bool wait_current(uint32_t generation, int seconds, int64_t deadline)
{
    for (int i = 0; i < seconds; ++i) {
        if (!current(generation) || esp_timer_get_time() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return current(generation);
}

static void pairing_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    auth_work_t *w = work_new();
    if (!w) { error_set(generation, "out_of_memory"); goto finished; }
    for (int waited = 0; (int64_t)time(NULL) < 1704067200LL && waited < 15; ++waited) {
        if (!current(generation)) goto finished;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!current(generation)) goto finished;
    if ((int64_t)time(NULL) < 1704067200LL) { error_set(generation, "clock_not_ready"); goto finished; }
    snprintf(w->form, sizeof(w->form), "client_id=" CLIENT_ID "&audience=https%%3A%%2F%%2Fapi.kombify.io&scope=openid%%20profile%%20email%%20offline_access");
    int status = 0, seconds = 0, interval = 5;
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    esp_err_t err = current(generation) ? post(w, CODE_URL, &status) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_http_lock);
    if (err != ESP_OK || status != 200) { error_set(generation, "device_authorization_failed"); goto finished; }
    cloud_auth_status_t view = {0};
    if (!json_field(w->response, "device_code", w->device, sizeof(w->device), NULL) || !w->device[0] ||
        !json_field(w->response, "user_code", view.user_code, sizeof(view.user_code), NULL) || !view.user_code[0] ||
        !json_field(w->response, "verification_uri", view.verification_uri, sizeof(view.verification_uri), NULL) ||
        strcmp(view.verification_uri, AUTH_ROOT "/activate") != 0 ||
        !json_field(w->response, "expires_in", NULL, 0, &seconds) || seconds <= 0 || seconds > 3600) {
        error_set(generation, "invalid_device_response"); goto finished;
    }
    if (!json_field(w->response, "interval", NULL, 0, &interval)) {
        if (json_has_field(w->response, "interval")) { error_set(generation, "invalid_poll_interval"); goto finished; }
        interval = 5;
    }
    if (interval < 5) interval = 5;
    if (interval > 3600) { error_set(generation, "invalid_poll_interval"); goto finished; }
    if (!json_field(w->response, "verification_uri_complete", view.verification_uri_complete, sizeof(view.verification_uri_complete), NULL)) {
        if (json_has_field(w->response, "verification_uri_complete")) { error_set(generation, "invalid_verification_uri"); goto finished; }
        if (!form_encode(view.user_code, w->encoded, sizeof(w->encoded))) goto finished;
        snprintf(view.verification_uri_complete, sizeof(view.verification_uri_complete), AUTH_ROOT "/activate?user_code=%.96s", w->encoded);
    }
    const char *complete_prefix = AUTH_ROOT "/activate?";
    if (strncmp(view.verification_uri_complete, complete_prefix, strlen(complete_prefix)) != 0) { error_set(generation, "invalid_verification_uri"); goto finished; }
    int64_t deadline = esp_timer_get_time() + (int64_t)seconds * 1000000;
    lock();
    if (s->generation == generation) {
        strlcpy(s->status.user_code, view.user_code, sizeof(s->status.user_code));
        strlcpy(s->status.verification_uri, view.verification_uri, sizeof(s->status.verification_uri));
        strlcpy(s->status.verification_uri_complete, view.verification_uri_complete, sizeof(s->status.verification_uri_complete));
        s->pairing_deadline_us = deadline;
        ESP_LOGI(TAG, "pair at %s code=%s", view.verification_uri, view.user_code);
    }
    unlock();
    while (current(generation) && esp_timer_get_time() < deadline) {
        if (!wait_current(generation, interval, deadline)) break;
        if (esp_timer_get_time() >= deadline) break;
        if (!form_encode(w->device, w->encoded, sizeof(w->encoded))) { error_set(generation, "device_code_too_long"); goto finished; }
        snprintf(w->form, sizeof(w->form), "client_id=" CLIENT_ID "&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code&device_code=%s", w->encoded);
        xSemaphoreTake(s_http_lock, portMAX_DELAY);
        err = current(generation) ? post(w, TOKEN_URL, &status) : ESP_ERR_INVALID_STATE;
        bool exchanged = err == ESP_OK;
        if (exchanged && status == 200 && current(generation)) err = accept_tokens(w, generation, false);
        xSemaphoreGive(s_http_lock);
        if (!current(generation)) break;
        if (!exchanged) {
            /* RFC 8628: back off on network timeout, still bounded by the
             * device-code deadline. No stale token is used while retrying. */
            error_set(generation, "pairing_transport_retry");
            interval = poll_backoff_interval(interval);
            continue;
        }
        if (status == 200) goto finished; /* accept_tokens recorded its error */
        if (poll_http_retryable(status)) {
            /* A proxy throttle or temporary provider failure may not be JSON.
             * Retain the code and retry only within its original deadline. */
            ESP_LOGW(TAG, "pairing poll retry http_status=%d error_code=oauth_http_retry", status);
            error_set(generation, "oauth_http_retry");
            interval = poll_backoff_interval(interval);
            continue;
        }
        char code[64] = "";
        if (!json_field(w->response, "error", code, sizeof(code), NULL)) {
            ESP_LOGW(TAG, "pairing poll terminal http_status=%d error_code=invalid_oauth_response", status);
            error_set(generation, "invalid_oauth_response"); goto finished;
        }
        if (!strcmp(code, "authorization_pending")) continue;
        if (!strcmp(code, "slow_down")) { interval += 5; continue; }
        const char *safe_code = safe_oauth_error(code);
        ESP_LOGW(TAG, "pairing poll terminal http_status=%d error_code=%s", status, safe_code);
        error_set(generation, safe_code);
        goto finished;
    }
    error_set(generation, "expired_token");
finished:
    work_free(w);
    lock();
    if (s->generation == generation) {
        s->status.pairing = false; s->status.user_code[0] = 0;
        s->status.verification_uri[0] = 0; s->status.verification_uri_complete[0] = 0;
    }
    s->worker_running = false;
    unlock();
    vTaskDeleteWithCaps(NULL);
}

esp_err_t cloud_auth_init(void)
{
    if (s) return ESP_OK;
    s_lock = xSemaphoreCreateMutex(); s_http_lock = xSemaphoreCreateMutex();
    auth_state_t *state = heap_caps_calloc(1, sizeof(*state), AUTH_CAPS);
    if (!s_lock || !s_http_lock || !state) {
        if (s_lock) vSemaphoreDelete(s_lock);
        if (s_http_lock) vSemaphoreDelete(s_http_lock);
        s_lock = NULL; s_http_lock = NULL; free(state); return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("cloud_auth", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t n = sizeof(state->refresh);
        err = nvs_get_str(h, "refresh", state->refresh, &n);
        nvs_close(h);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        wipe(state->refresh, sizeof(state->refresh));
        strlcpy(state->status.error_code, "credential_read_failed", sizeof(state->status.error_code));
    }
    state->status.paired = state->refresh[0] != 0;
    s = state;
    return ESP_OK;
}

esp_err_t cloud_auth_start_pairing(void)
{
    if (!s) return ESP_ERR_INVALID_STATE;
    lock();
    if (s->worker_running) { unlock(); return ESP_ERR_INVALID_STATE; }
    uint32_t generation = ++s->generation;
    s->worker_running = true; s->status.pairing = true;
    s->status.error_code[0] = 0; s->status.user_code[0] = 0; s->status.verification_uri[0] = 0;
    s->status.verification_uri_complete[0] = 0;
    s->pairing_deadline_us = 0;
    unlock();
    if (xTaskCreatePinnedToCoreWithCaps(pairing_task, "cloud_pair", 16384, (void *)(uintptr_t)generation, 4, NULL, 0, AUTH_CAPS) != pdPASS) {
        lock(); s->worker_running = false; s->status.pairing = false;
        strlcpy(s->status.error_code, "pairing_task_failed", sizeof(s->status.error_code)); unlock(); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t cloud_auth_forget(void)
{
    if (!s) return ESP_ERR_INVALID_STATE;
    lock();
    ++s->generation;
    wipe(s->access, sizeof(s->access)); wipe(s->refresh, sizeof(s->refresh));
    s->deadline_us = 0; s->pairing_deadline_us = 0; memset(&s->status, 0, sizeof(s->status));
    esp_err_t err = save_refresh("");
    if (err != ESP_OK) strlcpy(s->status.error_code, "forget_persist_failed", sizeof(s->status.error_code));
    unlock();
    return err;
}

esp_err_t cloud_auth_access_token(char *out, size_t capacity)
{
    if (!out || !capacity) return ESP_ERR_INVALID_ARG;
    out[0] = 0;
    if (!s) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_http_lock, portMAX_DELAY);
    lock();
    uint32_t generation = s->generation;
    if ((int64_t)time(NULL) < 1704067200LL) {
        strlcpy(s->status.error_code, "clock_not_ready", sizeof(s->status.error_code));
        unlock(); xSemaphoreGive(s_http_lock); return ESP_ERR_INVALID_STATE;
    }
    if (s->status.pairing || !s->refresh[0]) { unlock(); xSemaphoreGive(s_http_lock); return ESP_ERR_INVALID_STATE; }
    if (s->access[0] && s->deadline_us - esp_timer_get_time() > (int64_t)CLOUD_AUTH_RENEW_MARGIN_S * 1000000) {
        esp_err_t err = strlen(s->access) < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK) strlcpy(out, s->access, capacity);
        unlock(); xSemaphoreGive(s_http_lock); return err;
    }
    auth_work_t *w = work_new();
    if (!w) { strlcpy(s->status.error_code, "out_of_memory", sizeof(s->status.error_code)); unlock(); xSemaphoreGive(s_http_lock); return ESP_ERR_NO_MEM; }
    bool encoded = form_encode(s->refresh, w->encoded, sizeof(w->encoded));
    unlock();
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (encoded) {
        snprintf(w->form, sizeof(w->form), "client_id=" CLIENT_ID "&grant_type=refresh_token&refresh_token=%s", w->encoded);
        int status = 0;
        err = post(w, TOKEN_URL, &status);
        if (err == ESP_OK && status == 200) err = accept_tokens(w, generation, true);
        else if (err == ESP_OK) {
            char code[64] = "";
            if (json_field(w->response, "error", code, sizeof(code), NULL) && !strcmp(code, "invalid_grant")) invalidate(generation);
            else error_set(generation, "refresh_failed");
            err = ESP_FAIL;
        } else error_set(generation, "refresh_transport_failed");
    }
    if (err == ESP_OK) {
        lock();
        if (s->generation != generation || !s->access[0]) err = ESP_ERR_INVALID_STATE;
        else if (strlen(s->access) >= capacity) err = ESP_ERR_INVALID_SIZE;
        else strlcpy(out, s->access, capacity);
        unlock();
    }
    work_free(w);
    xSemaphoreGive(s_http_lock);
    return err;
}

int cloud_auth_access_valid_s(void)
{
    if (!s) return 0;
    lock();
    int64_t left = s->access[0] ? s->deadline_us - esp_timer_get_time() : 0;
    unlock();
    return left > 0 ? (int)(left / 1000000) : 0;
}

void cloud_auth_get_status(cloud_auth_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) { strlcpy(out->error_code, "not_initialized", sizeof(out->error_code)); return; }
    lock(); *out = s->status;
    int64_t left = (out->pairing ? s->pairing_deadline_us : s->deadline_us) - esp_timer_get_time();
    out->expires_in = left > 0 ? (int)(left / 1000000) : 0;
    unlock();
}
#endif
