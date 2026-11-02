/*
 * kombify box - edge device sync client. Contract: edge_sync.h.
 *
 * One task (PSRAM stack, like net_ws) wakes every second. Network work only
 * starts when the box is quiet (no conversation, no voice TLS, no firmware
 * transfer) and has been quiet for QUIET_HOLDOFF_US; each cycle opens one
 * HTTPS connection to Gateway, runs the due requests on it and closes it.
 * NVS writes go through the timer task, never this task.
 */
#include "edge_sync.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "audio.h"
#include "board_identity.h"
#include "cloud_auth.h"
#include "device_config.h"
#include "fw_ota.h"
#include "fw_update_manifest.h"
#include "i18n.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "secure_zero.h"

static const char *TAG = "kbx_sync";

#define NVS_NS            "kbx_edge"
#define NVS_KEY_HANDLED   "handled_v1"   /* legacy u32, read once for migration */
#define NVS_KEY_DEVID     "devid_v1"     /* legacy str, read once for migration */
#define NVS_KEY_COUNTER   "handled_v2"   /* blob: counter_t, one record */

#define US(s)                 ((int64_t)(s) * 1000000)
#define POLL_PERIOD_US        US(60)
#define POLL_HINT_MIN_S       5          /* x-kombify-poll-after, clamped */
#define POLL_HINT_MAX_S       300
#define CHECKIN_MIN_US        US(30)
#define CHECKIN_PERIOD_US     US(300)
#define AUTO_UPDATE_PERIOD_US US(6 * 3600)
#define AUTO_UPDATE_FIRST_US  US(180)
#define QUIET_HOLDOFF_US      US(10)
#define UPDATE_RETRY_US       US(60)
#define VOLUME_DEBOUNCE_US    US(4)
#define WRITE_REJECT_HOLD_US  US(600)
#define BACKOFF_MIN_S         30
#define BACKOFF_MAX_S         300
#define HTTP_TIMEOUT_MS       15000
/* Internal RAM for one HTTPS cycle. mbedTLS allocates in PSRAM
 * (MBEDTLS_EXTERNAL_MEM_ALLOC) and malloc() above 512 B prefers PSRAM
 * (SPIRAM_MALLOC_ALWAYSINTERNAL), so the esp-tls context and the HTTP buffers
 * live there. What stays internal is small and scattered: the PSA SHA
 * driver's hash contexts (DMA-capable internal, a few hundred bytes each,
 * during the handshake), lwIP's socket mailbox and semaphores, Wi-Fi's DMA
 * frame buffers (1.7 KB each) and esp_http_client's bookkeeping. So a cycle
 * needs one modest block and enough free RAM that the wake word engine keeps
 * its floor (wakeword.c MIN_INTERNAL_KEEP); it never waits for a single 12 KB
 * block, which a fragmented heap (largest 7.5 KB at 25 KB free) never offers.
 * The cycle measures what it took and the next one gates on that. */
#define INTERNAL_BLOCK_MIN    2048u
#define WAKE_KEEP_FLOOR       (10u * 1024u)
#define CYCLE_COST_GUESS      4096u      /* until a cycle has measured itself */
#define CYCLE_COST_MIN        2048u
#define CYCLE_COST_MAX        (12u * 1024u)
#define CYCLE_MARGIN          1024u
#define WARM_YIELD_AFTER_US   POLL_PERIOD_US /* low heap this long: the warm voice path yields */
#define WARM_YIELD_WAIT_US    US(20)     /* time it gets to close its sockets */
#define WARM_HOLD_MAX_US      US(90)     /* a hold never outlasts this (a turn ran meanwhile) */
#define STALE_CONFIRM_US      US(180)    /* a lower account revision this long is real */

#define ID_MAX     56
#define SUB_MAX    192
#define RESP_CAP   12288
#define REQ_CAP    3072
#define CLAIMS_CAP 6144

typedef struct {
    char token[CLOUD_AUTH_TOKEN_MAX];
    char auth[CLOUD_AUTH_TOKEN_MAX + 16];
    char claims[CLAIMS_CAP];
    char url[200];
    char req[REQ_CAP];
    char reported[1536];
    char resp[RESP_CAP];
} work_t;

typedef struct {
    char *buf;
    size_t cap, len;
    bool overflow;
    char etag[24];
    int poll_after_s;               /* x-kombify-poll-after header, 0 = absent */
} resp_ctx_t;

typedef struct {
    esp_http_client_handle_t client;
    work_t *w;
    resp_ctx_t ctx;
    char id[ID_MAX];
    int status;
    bool transport_failed;
} cycle_t;

static TaskHandle_t s_task;
static volatile bool s_poke;
/* update_request already acted on, bound to the device id it counts for. One
 * record in RAM (guarded by s_ctr_lock) and in NVS (one blob), committed
 * synchronously before an install can start. */
typedef struct {
    uint32_t handled;
    char id[ID_MAX];
} counter_t;
static counter_t s_ctr;
static portMUX_TYPE s_ctr_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_next_update_try_us;   /* deferred update: not before this */

static uint32_t counter_handled(void)
{
    taskENTER_CRITICAL(&s_ctr_lock);
    uint32_t v = s_ctr.handled;
    taskEXIT_CRITICAL(&s_ctr_lock);
    return v;
}

static bool counter_has_id(const char *id)
{
    taskENTER_CRITICAL(&s_ctr_lock);
    bool same = strcmp(s_ctr.id, id) == 0;
    taskEXIT_CRITICAL(&s_ctr_lock);
    return same;
}

/* Durable before it returns: the RAM copy changes only after the commit. */
static esp_err_t counter_commit(uint32_t handled, const char *id)
{
    counter_t c;
    memset(&c, 0, sizeof(c));
    c.handled = handled;
    strlcpy(c.id, id, sizeof(c.id));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_COUNTER, &c, sizeof(c));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sync: update counter not saved (%s)", esp_err_to_name(err));
        return err;
    }
    taskENTER_CRITICAL(&s_ctr_lock);
    s_ctr = c;
    taskEXIT_CRITICAL(&s_ctr_lock);
    return ESP_OK;
}

/* Sync state (sync task only, except s_block read by the console). */
static int64_t s_last_busy_us, s_last_checkin_us, s_last_poll_us, s_next_try_us, s_write_hold_us;
static int64_t s_next_auto_us, s_chg_us;
static int64_t s_poll_period_us = POLL_PERIOD_US;   /* the Gateway's poll hint, else 60 s */
static uint32_t s_report_hash;
static bool s_have_checkin;
static bool s_seen_valid;
static uint32_t s_seen_rev;
static uint32_t s_rejected_rev;
static char s_reject_reason[48];
static int s_backoff_s;
static bool s_report_installing;
static const char *volatile s_block = "starting";
static int s_last_status;
static char s_last_id[ID_MAX];
/* Low internal RAM: since when, and the warm voice path's yield for it. */
static int64_t s_low_heap_since_us, s_hold_since_us, s_yield_retry_us;
static bool s_warm_held;
static uint32_t s_yields;
/* Internal RAM one cycle took (measured), and the lowest free seen in the
 * running cycle (sampled at each HTTP event and request). */
static uint32_t s_cycle_cost = CYCLE_COST_GUESS;
static size_t s_cycle_low;

static void cycle_sample_heap(void)
{
    size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (f < s_cycle_low) s_cycle_low = f;
}

static size_t internal_free_needed(void)
{
    return WAKE_KEEP_FLOOR + s_cycle_cost + CYCLE_MARGIN;
}
/* An account revision below the applied one, first seen at s_stale_first_us. */
static uint32_t s_stale_rev;
static int64_t s_stale_first_us;

typedef struct {
    unsigned volume;
    bool route_agent;
    char locale[12];
} snap_t;
static snap_t s_snap, s_cand;
static bool s_snap_valid, s_chg_active;
#define HASH_BUF 1536
static char *s_hash_buf;

/* ------------------------------------------------------------ helpers */

const char *edge_sync_gateway_base(void)
{
    return EDGE_SYNC_BASE_DEFAULT;
}

/* HMAC-SHA256 on the PSA hash (mbedTLS 4 exposes hashes through PSA). */
static bool hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                        uint8_t out[32])
{
    uint8_t pad[64], inner[32];
    size_t n = 0;
    if (key_len > sizeof(pad) || psa_crypto_init() != PSA_SUCCESS) return false;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    memset(pad, 0x36, sizeof(pad));
    for (size_t i = 0; i < key_len; i++) pad[i] ^= key[i];
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS &&
              psa_hash_update(&op, pad, sizeof(pad)) == PSA_SUCCESS &&
              psa_hash_update(&op, msg, msg_len) == PSA_SUCCESS &&
              psa_hash_finish(&op, inner, sizeof(inner), &n) == PSA_SUCCESS && n == 32;
    psa_hash_abort(&op);
    if (!ok) return false;
    memset(pad, 0x5c, sizeof(pad));
    for (size_t i = 0; i < key_len; i++) pad[i] ^= key[i];
    op = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS &&
         psa_hash_update(&op, pad, sizeof(pad)) == PSA_SUCCESS &&
         psa_hash_update(&op, inner, sizeof(inner)) == PSA_SUCCESS &&
         psa_hash_finish(&op, out, 32, &n) == PSA_SUCCESS && n == 32;
    psa_hash_abort(&op);
    secure_zero(pad, sizeof(pad));
    secure_zero(inner, sizeof(inner));
    return ok;
}

static int b64url_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Decoded length, or -1. out is NUL terminated; JSON never carries NUL. */
static int b64url_decode(const char *in, size_t len, char *out, size_t cap)
{
    if (!len || len % 4 == 1 || !cap) return -1;
    uint32_t acc = 0;
    unsigned bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64url_value((unsigned char)in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            unsigned char byte = (unsigned char)(acc >> bits);
            if (!byte || n + 1 >= cap) return -1;
            out[n++] = (char)byte;
        }
    }
    out[n] = 0;
    return (int)n;
}

static void b64url_encode(const uint8_t *in, size_t len, char *out)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = tab[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = tab[v & 63];
    }
    out[o] = 0;
}

/* sub claim of the box's own access token (payload only; Gateway verifies
 * the signature, the box only needs the subject for its device id). */
static bool token_subject(const char *token, char *claims, size_t cap, char *sub, size_t sub_cap)
{
    const char *first = strchr(token, '.');
    const char *second = first ? strchr(first + 1, '.') : NULL;
    if (!first || !second || second == first + 1) {
        ESP_LOGW(TAG, "sync: access token is not a JWT");
        return false;
    }
    if (b64url_decode(first + 1, (size_t)(second - first - 1), claims, cap) < 0) {
        ESP_LOGW(TAG, "sync: token payload undecodable (%u chars)", (unsigned)(second - first - 1));
        return false;
    }
    cJSON *root = cJSON_Parse(claims);
    const cJSON *item = root ? cJSON_GetObjectItemCaseSensitive(root, "sub") : NULL;
    bool ok = false;
    if (cJSON_IsString(item) && item->valuestring[0] && strlen(item->valuestring) < sub_cap) {
        strlcpy(sub, item->valuestring, sub_cap);
        ok = true;
    }
    if (!ok) ESP_LOGW(TAG, "sync: token has no usable sub claim (payload %u bytes)", (unsigned)strlen(claims));
    cJSON_Delete(root);
    return ok;
}

static bool derive_device_id(const char *sub, char *id, size_t cap)
{
    uint8_t mac[6], mac_hmac[32];
    char msg[SUB_MAX + 32];
    if (cap < 48 || esp_efuse_mac_get_default(mac) != ESP_OK) return false;
    int n = snprintf(msg, sizeof(msg), "kombify-connect-device:v1:%s", sub);
    if (n <= 0 || (size_t)n >= sizeof(msg)) return false;
    bool ok = hmac_sha256(mac, sizeof(mac), (const uint8_t *)msg, (size_t)n, mac_hmac);
    if (ok) {
        memcpy(id, "dev_", 4);
        b64url_encode(mac_hmac, sizeof(mac_hmac), id + 4);
    }
    secure_zero(mac, sizeof(mac));
    secure_zero(mac_hmac, sizeof(mac_hmac));
    return ok;
}

static const char *update_state_name(fw_ota_update_state_t s)
{
    switch (s) {
    case FW_OTA_UPDATE_CHECKING: return "checking";
    case FW_OTA_UPDATE_CURRENT: return "current";
    case FW_OTA_UPDATE_AVAILABLE: return "available";
    case FW_OTA_UPDATE_INSTALLING: return "installing";
    case FW_OTA_UPDATE_REBOOTING: return "rebooting";
    case FW_OTA_UPDATE_ERROR: return "error";
    case FW_OTA_UPDATE_IDLE:
    default: return "idle";
    }
}

static void sanitize(char *s)
{
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr(" ._:-", *s)) *s = '_';
    }
}

static uint32_t fnv(uint32_t h, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* The reported state whose change triggers a check-in (not the progress). */
static uint32_t report_hash(char *scratch, size_t cap)
{
    uint32_t h = 2166136261u;
    size_t n = device_config_reported_body(scratch, cap);
    h = fnv(h, scratch, n);
    device_config_state_t st;
    device_config_get_state(&st);
    fw_ota_update_status_t up;
    fw_ota_get_update_status(&up);
    uint32_t extra[6] = { st.applied_revision, st.local_pending, s_rejected_rev, (uint32_t)up.state,
                          counter_handled(), s_report_installing };
    h = fnv(h, extra, sizeof(extra));
    h = fnv(h, up.version, strlen(up.version));
    return h;
}

/* ----------------------------------------------------------- HTTP */

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    cycle_sample_heap();
    resp_ctx_t *c = e->user_data;
    if (!c) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        !strcasecmp(e->header_key, "ETag")) {
        strlcpy(c->etag, e->header_value, sizeof(c->etag));
    } else if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
               !strcasecmp(e->header_key, "x-kombify-poll-after")) {
        c->poll_after_s = atoi(e->header_value);
    } else if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        if (c->len + (size_t)e->data_len < c->cap) {
            memcpy(c->buf + c->len, e->data, (size_t)e->data_len);
            c->len += (size_t)e->data_len;
            c->buf[c->len] = 0;
        } else {
            c->overflow = true;
        }
    }
    return ESP_OK;
}

/* Next poll period from the Gateway's hint, clamped; 0 = absent -> 60 s. */
static void note_poll_hint(int seconds)
{
    int64_t period = POLL_PERIOD_US;
    if (seconds > 0) {
        if (seconds < POLL_HINT_MIN_S) seconds = POLL_HINT_MIN_S;
        if (seconds > POLL_HINT_MAX_S) seconds = POLL_HINT_MAX_S;
        period = US(seconds);
    }
    if (period != s_poll_period_us) {
        ESP_LOGI(TAG, "sync: poll period %d s", (int)(period / 1000000));
        s_poll_period_us = period;
    }
}

/* One request on the cycle's connection. Returns the HTTP status or 0. */
static int gw_call(cycle_t *cy, esp_http_client_method_t method, const char *suffix,
                   const char *if_match_rev, const char *if_none_match_rev, const char *body)
{
    work_t *w = cy->w;
    snprintf(w->url, sizeof(w->url), "%s/v1/connect/edge-devices/%s%s", edge_sync_gateway_base(), cy->id, suffix);
    cy->ctx.len = 0;
    cy->ctx.overflow = false;
    cy->ctx.etag[0] = 0;
    cy->ctx.poll_after_s = 0;
    cy->ctx.buf[0] = 0;
    if (!cy->client) {
        esp_http_client_config_t cfg = {
            .url = w->url, .method = method, .timeout_ms = HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,      /* the token only ever reaches the pinned origin */
            .keep_alive_enable = true,
            .buffer_size = 2048, .buffer_size_tx = CLOUD_AUTH_TOKEN_MAX + 1024, /* the JWT is ~5 KB */
            .event_handler = http_evt, .user_data = &cy->ctx,
            .user_agent = "kombify-box-sync/1",
        };
        cy->client = esp_http_client_init(&cfg);
        if (!cy->client) return 0;
    } else if (esp_http_client_set_url(cy->client, w->url) != ESP_OK) {
        return 0;
    }
    esp_http_client_set_method(cy->client, method);
    esp_http_client_set_header(cy->client, "Authorization", w->auth);
    esp_http_client_set_header(cy->client, "Accept", "application/json");
    esp_http_client_set_header(cy->client, "Content-Type", "application/json");
    char tag[24];
    if (if_match_rev) {
        snprintf(tag, sizeof(tag), "\"v%s\"", if_match_rev);
        esp_http_client_set_header(cy->client, "If-Match", tag);
    } else {
        esp_http_client_delete_header(cy->client, "If-Match");
    }
    if (if_none_match_rev) {
        snprintf(tag, sizeof(tag), "\"v%s\"", if_none_match_rev);
        esp_http_client_set_header(cy->client, "If-None-Match", tag);
    } else {
        esp_http_client_delete_header(cy->client, "If-None-Match");
    }
    esp_http_client_set_post_field(cy->client, body, body ? (int)strlen(body) : 0);
    esp_err_t err = esp_http_client_perform(cy->client);
    cycle_sample_heap();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sync: %s transport %s", suffix[0] ? "settings" : "check-in", esp_err_to_name(err));
        cy->transport_failed = true;
        return 0;
    }
    cy->status = esp_http_client_get_status_code(cy->client);
    s_last_status = cy->status;
    /* The Gateway asks for faster polling while someone has the device's
     * settings open (10 s), else 60 s; no header (an older Gateway) = 60 s. */
    if (method == HTTP_METHOD_GET || suffix[0] == '\0') {
        if (cy->status == 200 || cy->status == 304 || (cy->status > 200 && cy->status < 300)) {
            note_poll_hint(cy->ctx.poll_after_s);
        }
    }
    return cy->status;
}

/* "v12" / "\"v12\"" -> 12, else 0. */
static uint32_t etag_revision(const char *etag)
{
    const char *p = strchr(etag, 'v');
    return p ? (uint32_t)strtoul(p + 1, NULL, 10) : 0;
}

static void log_denial(const char *what, cycle_t *cy)
{
    char code[64] = "";
    cJSON *root = cy->ctx.len ? cJSON_Parse(cy->ctx.buf) : NULL;
    const cJSON *rc = root ? cJSON_GetObjectItemCaseSensitive(root, "reason_code") : NULL;
    if (cJSON_IsString(rc)) strlcpy(code, rc->valuestring, sizeof(code));
    cJSON_Delete(root);
    sanitize(code);
    ESP_LOGW(TAG, "sync: %s refused http=%d reason=%s", what, cy->status, code);
}

/* ------------------------------------------------- settings document */

static void snap_take(void)
{
    s_snap.volume = audio_volume();
    s_snap.route_agent = net_ws_route_agent();
    strlcpy(s_snap.locale, i18n_locale_tag(i18n_requested_locale()), sizeof(s_snap.locale));
    s_snap_valid = true;
    s_chg_active = false;
}

static bool snap_differs(const snap_t *a)
{
    return a->volume != audio_volume() || a->route_agent != net_ws_route_agent() ||
           strcmp(a->locale, i18n_locale_tag(i18n_requested_locale())) != 0;
}

/* Volume, route and locale have no change hook: notice them here. The
 * volume slider is debounced so a drag is one write-back. */
static void local_watch(int64_t now)
{
    if (!s_snap_valid) {
        snap_take();
        return;
    }
    if (!snap_differs(&s_snap)) {
        s_chg_active = false;
        return;
    }
    if (!s_chg_active || snap_differs(&s_cand)) {
        s_cand.volume = audio_volume();
        s_cand.route_agent = net_ws_route_agent();
        strlcpy(s_cand.locale, i18n_locale_tag(i18n_requested_locale()), sizeof(s_cand.locale));
        s_chg_active = true;
        s_chg_us = now;
        return;
    }
    if (now - s_chg_us >= VOLUME_DEBOUNCE_US) {
        device_config_note_local_change();
        snap_take();
    }
}

/* The account serves a revision below the one this box applied. Once is a
 * lagging read and is ignored; the same revision STALE_CONFIRM_US later means
 * the account's numbering restarted (or a bench apply ran ahead of it). Left
 * alone the box would wait forever: no newer document comes, and a pending
 * local change is written only on the applied revision. */
static bool stale_confirmed(uint32_t revision)
{
    int64_t now = esp_timer_get_time();
    if (s_stale_rev != revision) {
        s_stale_rev = revision;
        s_stale_first_us = now;
        ESP_LOGI(TAG, "sync: account serves revision %lu, below the applied one; adopted if it stays %d s",
                 (unsigned long)revision, (int)(STALE_CONFIRM_US / 1000000));
        return false;
    }
    if (now - s_stale_first_us < STALE_CONFIRM_US) return false;
    s_stale_rev = 0;
    return true;
}

/* Accepts one account document (also the revision 0 "unset" document).
 * Returns true when the box must seed the account from its own values. */
static bool take_settings(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *rev = root ? cJSON_GetObjectItemCaseSensitive(root, "revision") : NULL;
    bool zero = cJSON_IsNumber(rev) && rev->valuedouble == 0.0;
    cJSON_Delete(root);
    if (zero) {
        s_seen_valid = true;
        s_seen_rev = 0;
        ESP_LOGI(TAG, "sync: account has no settings document (revision 0), seeding from this box");
        return true;
    }
    const char *reason = NULL;
    uint32_t revision = 0;
    device_config_state_t before;
    device_config_get_state(&before);
    device_config_outcome_t outcome = device_config_apply_json_rev(json, len, &reason, &revision);
    if (outcome == DEVICE_CONFIG_STALE && revision && revision < before.applied_revision &&
        stale_confirmed(revision) && !device_config_rebase(revision)) {
        /* Nothing local to keep: the account's document applies. */
        outcome = device_config_apply_json_rev(json, len, &reason, &revision);
    }
    if (outcome == DEVICE_CONFIG_APPLIED || outcome == DEVICE_CONFIG_CURRENT) s_stale_rev = 0;
    if (revision) {
        s_seen_valid = true;
        s_seen_rev = revision;
    }
    if (outcome == DEVICE_CONFIG_REJECTED) {
        s_rejected_rev = revision;
        strlcpy(s_reject_reason, reason ? reason : "invalid", sizeof(s_reject_reason));
        sanitize(s_reject_reason);
    } else if (outcome == DEVICE_CONFIG_APPLIED || outcome == DEVICE_CONFIG_CURRENT) {
        s_rejected_rev = 0;
        s_reject_reason[0] = 0;
    }
    if (outcome == DEVICE_CONFIG_APPLIED) snap_take();
    return false;
}

/* ------------------------------------------------------- requests */

static const char *firmware_state_for_report(fw_ota_update_status_t *up)
{
    fw_ota_get_update_status(up);
    return s_report_installing ? "installing" : update_state_name(up->state);
}

static int build_checkin(work_t *w, bool *ok)
{
    char ip[16] = "";
    net_wifi_is_connected(ip, sizeof(ip));
    size_t n = device_config_reported_body(w->reported, sizeof(w->reported));
    device_config_state_t st;
    device_config_get_state(&st);
    fw_ota_update_status_t up;
    const char *state = firmware_state_for_report(&up);
    char detail[sizeof(up.detail)], version[sizeof(up.version)];
    strlcpy(detail, up.detail, sizeof(detail));
    strlcpy(version, up.version, sizeof(version));
    sanitize(detail);
    sanitize(version);
    char sync[160];
    int sn = snprintf(sync, sizeof(sync), "{\"applied_revision\":%lu,\"local_pending\":%s",
                      (unsigned long)st.applied_revision, st.local_pending ? "true" : "false");
    if (s_rejected_rev && sn > 0 && (size_t)sn < sizeof(sync)) {
        sn += snprintf(sync + sn, sizeof(sync) - (size_t)sn, ",\"rejected_revision\":%lu,\"reject_reason\":\"%s\"",
                       (unsigned long)s_rejected_rev, s_reject_reason);
    }
    if (sn > 0 && (size_t)sn < sizeof(sync) - 2) {
        sync[sn++] = '}';
        sync[sn] = 0;
    }
    *ok = n > 0;
    if (!n) return 0;
    return snprintf(w->req, sizeof(w->req),
                    "{\"schema\":\"kombify.edge_device.checkin.v1\",\"kind\":\"kombify-box\",\"board\":\"%s"
                    "\",\"firmware_version\":\"%s\",\"display_name\":\"kombify Box\"%s%s%s,\"reported\":%s,\"sync\":%s,"
                    "\"firmware_update\":{\"state\":\"%s\"%s%s%s,\"detail\":\"%s\",\"progress_percent\":%u,\"handled_request\":%lu}}",
                    kbx_board_effective_id(), esp_app_get_description()->version,
                    ip[0] ? ",\"local_ip\":\"" : "", ip, ip[0] ? "\"" : "", w->reported, sync, state,
                    version[0] ? ",\"version\":\"" : "", version, version[0] ? "\"" : "", detail,
                    up.progress_percent, (unsigned long)counter_handled());
}

static bool do_checkin(cycle_t *cy, bool *seed)
{
    bool ok = false;
    int n = build_checkin(cy->w, &ok);
    if (!ok || n <= 0 || (size_t)n >= REQ_CAP) {
        ESP_LOGW(TAG, "sync: check-in body unavailable");
        return false;
    }
    uint32_t hash = report_hash(cy->w->reported, sizeof(cy->w->reported));
    int status = gw_call(cy, HTTP_METHOD_PUT, "", NULL, NULL, cy->w->req);
    if (status < 200 || status > 299) {
        if (status) log_denial("check-in", cy);
        return false;
    }
    s_have_checkin = true;
    s_last_checkin_us = esp_timer_get_time();
    s_last_poll_us = s_last_checkin_us;
    s_report_hash = hash;
    cJSON *root = cy->ctx.overflow ? NULL : cJSON_Parse(cy->ctx.buf);
    const cJSON *settings = root ? cJSON_GetObjectItemCaseSensitive(root, "settings") : NULL;
    const cJSON *poll_after = root ? cJSON_GetObjectItemCaseSensitive(root, "poll_after_seconds") : NULL;
    if (cJSON_IsNumber(poll_after) && !cy->ctx.poll_after_s) note_poll_hint(poll_after->valueint);
    device_config_state_t before;
    device_config_get_state(&before);
    ESP_LOGI(TAG, "sync: check-in http=%d applied=%lu pending=%d", status, (unsigned long)before.applied_revision,
             (int)before.local_pending);
    /* board_mismatch: the box reports, it applies no account settings (no
     * board driver runs that could take them). */
    if (cJSON_IsObject(settings) && !kbx_board_mismatch()) {
        char *doc = cJSON_PrintUnformatted(settings);
        if (doc) {
            *seed = take_settings(doc, strlen(doc));
            free(doc);
        }
    }
    cJSON_Delete(root);
    return true;
}

static bool do_poll(cycle_t *cy, bool *seed, bool *need_checkin)
{
    char rev[12] = "";
    if (s_seen_valid) snprintf(rev, sizeof(rev), "%lu", (unsigned long)s_seen_rev);
    int status = gw_call(cy, HTTP_METHOD_GET, "/settings", NULL, rev[0] ? rev : NULL, NULL);
    s_last_poll_us = esp_timer_get_time();
    if (status == 304) {
        ESP_LOGI(TAG, "sync: settings http=304 (revision %s)", rev);
        device_config_state_t st;
        device_config_get_state(&st);
        if (s_seen_valid && s_seen_rev < st.applied_revision && stale_confirmed(s_seen_rev) &&
            !device_config_rebase(s_seen_rev)) {
            s_seen_valid = false;      /* fetch the document to apply it */
            s_last_poll_us = 0;
        }
        return true;
    }
    if (status == 404) {
        s_have_checkin = false;        /* Gateway has no record yet: check in next */
        *need_checkin = true;
        ESP_LOGI(TAG, "sync: settings http=404, checking in");
        return true;
    }
    if (status < 200 || status > 299 || cy->ctx.overflow) {
        if (status) log_denial("settings", cy);
        return false;
    }
    ESP_LOGI(TAG, "sync: settings http=%d", status);
    *seed = take_settings(cy->ctx.buf, cy->ctx.len);
    return true;
}

/* PUT the local values with If-Match: a pending local change, or the seed of
 * a revision 0 account. */
static bool do_write(cycle_t *cy, bool seed)
{
    /* The snapshot precedes the body: a local edit after it stays pending. */
    device_config_state_t st;
    device_config_get_state(&st);
    size_t n = device_config_writeback_json(cy->w->req, REQ_CAP);
    if (!n) {
        ESP_LOGW(TAG, "sync: write-back body unavailable");
        return false;
    }
    char rev[12];
    snprintf(rev, sizeof(rev), "%lu", (unsigned long)(seed ? 0 : st.applied_revision));
    int status = gw_call(cy, HTTP_METHOD_PUT, "/settings", rev, NULL, cy->w->req);
    if (status == 412) {
        ESP_LOGI(TAG, "sync: write-back http=412, the account moved on; its newer document wins");
        s_seen_valid = false;
        s_last_poll_us = 0;
        return true;
    }
    if (status == 400) {
        log_denial("write-back", cy);
        s_write_hold_us = esp_timer_get_time() + WRITE_REJECT_HOLD_US;
        return true;
    }
    if (status < 200 || status > 299) {
        if (status) log_denial("write-back", cy);
        return false;
    }
    uint32_t revision = etag_revision(cy->ctx.etag);
    if (!revision && !cy->ctx.overflow) {
        cJSON *root = cJSON_Parse(cy->ctx.buf);
        const cJSON *r = root ? cJSON_GetObjectItemCaseSensitive(root, "revision") : NULL;
        if (!cJSON_IsNumber(r) && root) {
            const cJSON *settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
            r = cJSON_IsObject(settings) ? cJSON_GetObjectItemCaseSensitive(settings, "revision") : NULL;
        }
        if (cJSON_IsNumber(r) && r->valuedouble >= 1) revision = (uint32_t)r->valuedouble;
        cJSON_Delete(root);
    }
    if (!revision) {
        ESP_LOGW(TAG, "sync: write-back http=%d without a revision", status);
        s_seen_valid = false;
        return true;
    }
    s_seen_valid = true;
    s_seen_rev = revision;
    if (seed) device_config_mark_seeded(revision, &st);
    else device_config_mark_written(revision, &st);
    device_config_state_t after;
    device_config_get_state(&after);
    if (after.generation != st.generation) s_seen_valid = false;   /* reset meanwhile: ask again */
    ESP_LOGI(TAG, "sync: %s http=%d account at revision %lu", seed ? "seed" : "write-back", status,
             (unsigned long)revision);
    return true;
}

/* ------------------------------------------------------ the cycle */

static void cycle_free(cycle_t *cy)
{
    if (cy->client) esp_http_client_cleanup(cy->client);
    if (cy->w) {
        secure_zero(cy->w->token, sizeof(cy->w->token));
        secure_zero(cy->w->auth, sizeof(cy->w->auth));
        free(cy->w);
    }
    cy->client = NULL;
    cy->w = NULL;
}

static void note_failure(int64_t now)
{
    s_backoff_s = s_backoff_s ? s_backoff_s * 2 : BACKOFF_MIN_S;
    if (s_backoff_s > BACKOFF_MAX_S) s_backoff_s = BACKOFF_MAX_S;
    s_next_try_us = now + US(s_backoff_s);
    ESP_LOGW(TAG, "sync: retry in %d s", s_backoff_s);
}

static bool adopt_device_id(const char *id)
{
    /* The handled counter belongs to one device id: another account or a
     * fresh pairing restarts it. */
    if (!counter_has_id(id)) {
        ESP_LOGI(TAG, "sync: device id changed, update counter restarts");
        (void)counter_commit(0, id);
    }
    if (strcmp(s_last_id, id) != 0) {
        strlcpy(s_last_id, id, sizeof(s_last_id));
        s_have_checkin = false;
        s_seen_valid = false;
        ESP_LOGI(TAG, "sync: device %s", id);
    }
    return true;
}

typedef struct {
    bool checkin, poll, write, bench;
} due_t;

/* Bench writer (KBX sync put): one settings PUT as an outside writer would
 * send it, by the box's own identity, so the pull path can be proven against
 * a real Gateway without a second client. It never touches device_config:
 * the box learns the new revision by its next poll like any other change. */
static volatile bool s_put_pending;
#if CONFIG_KBX_DEV_ENDPOINTS
static char s_put_body[1536];
static char s_put_rev[12];

static bool do_bench_put(cycle_t *cy)
{
    char rev[12];
    strlcpy(rev, s_put_rev[0] ? s_put_rev : "0", sizeof(rev));
    if (!s_put_rev[0] && s_seen_valid) snprintf(rev, sizeof(rev), "%lu", (unsigned long)s_seen_rev);
    /* The cycle sends its own copy: a console command may queue the next
     * put as soon as this one is no longer pending. */
    strlcpy(cy->w->req, s_put_body, REQ_CAP);
    s_put_pending = false;
    int status = gw_call(cy, HTTP_METHOD_PUT, "/settings", rev, NULL, cy->w->req);
    char reply[121] = "";
    strlcpy(reply, cy->ctx.buf, sizeof(reply));
    sanitize(reply);
    ESP_LOGI(TAG, "sync: bench put if-match v%s http=%d etag=%s %.120s", rev, status, cy->ctx.etag, reply);
    return status != 0;
}
#endif

static bool run_cycle(int64_t now, due_t due)
{
    cycle_t cy = {0};
    cy.w = heap_caps_calloc(1, sizeof(work_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cy.w) {
        ESP_LOGW(TAG, "sync: no memory for a cycle");
        note_failure(now);
        return false;
    }
    cy.ctx.buf = cy.w->resp;
    cy.ctx.cap = RESP_CAP;
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_cycle_low = free_before;
    char sub[SUB_MAX];
    /* May refresh the token: a TLS connection of its own, same cost class. */
    bool ok = cloud_auth_access_token(cy.w->token, sizeof(cy.w->token)) == ESP_OK;
    cycle_sample_heap();
    if (!ok) {
        ESP_LOGW(TAG, "sync: no access token yet");
    } else if (!token_subject(cy.w->token, cy.w->claims, sizeof(cy.w->claims), sub, sizeof(sub)) ||
               !derive_device_id(sub, cy.id, sizeof(cy.id))) {
        ESP_LOGW(TAG, "sync: device id unavailable (token has no subject)");
        ok = false;
    }
    secure_zero(sub, sizeof(sub));
    if (ok) {
        snprintf(cy.w->auth, sizeof(cy.w->auth), "Bearer %s", cy.w->token);
        secure_zero(cy.w->claims, sizeof(cy.w->claims));
        adopt_device_id(cy.id);
        if (!s_have_checkin) due.checkin = true;
    }
    bool seed = false, need_checkin = false;
    if (kbx_board_mismatch()) {
        due.poll = false;
        due.write = false;
    }
    ESP_LOGI(TAG, "sync: cycle checkin=%d poll=%d write=%d heap_int=%u largest=%u", (int)due.checkin, (int)due.poll,
             (int)due.write, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (ok && due.checkin) ok = do_checkin(&cy, &seed);
    else if (ok && due.poll) ok = do_poll(&cy, &seed, &need_checkin);
    if (ok && need_checkin) ok = do_checkin(&cy, &seed);
    device_config_state_t st;
    device_config_get_state(&st);
    /* Seed an account without a document; write a local change only on the
     * revision the box applied, otherwise the account's newer document wins. */
    bool write = seed || (st.local_pending && s_seen_valid && s_seen_rev == st.applied_revision &&
                          esp_timer_get_time() >= s_write_hold_us);
    if (ok && write && !kbx_board_mismatch()) ok = do_write(&cy, seed);
#if CONFIG_KBX_DEV_ENDPOINTS
    if (ok && due.bench && s_put_pending) ok = do_bench_put(&cy);
#endif
    cycle_free(&cy);
    /* What this cycle took from internal RAM gates the next one. */
    uint32_t cost = free_before > s_cycle_low ? (uint32_t)(free_before - s_cycle_low) : 0;
    s_cycle_cost = cost < CYCLE_COST_MIN ? CYCLE_COST_MIN : cost > CYCLE_COST_MAX ? CYCLE_COST_MAX : cost;
    ESP_LOGI(TAG, "sync: cycle %s, internal RAM %u -> low %u (cost %u B)", ok ? "done" : "failed",
             (unsigned)free_before, (unsigned)s_cycle_low, (unsigned)cost);
    if (ok) {
        s_backoff_s = 0;
        s_next_try_us = 0;
    } else {
        note_failure(now);
    }
    return ok;
}

/* -------------------------------------------------------- firmware */

static bool wait_update_state(fw_ota_update_state_t from, int seconds, fw_ota_update_status_t *out)
{
    for (int i = 0; i < seconds * 2; i++) {
        fw_ota_get_update_status(out);
        if (out->state != from) return true;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

static const char *busy_reason(void);

typedef enum {
    UPDATE_CONCLUDED = 0,   /* nothing to install, an error, or the install ran */
    UPDATE_DEFERRED,        /* not now: try again later, the request stays open */
} update_result_t;

/* The policy the check ran under still holds: same pin, and the request or
 * the auto_update switch that started it is still current. */
static bool update_still_wanted(const char *target_used, bool manual, uint32_t request)
{
    device_config_firmware_t fw;
    device_config_firmware(&fw);
    if (!fw.valid) return false;
    if (strcmp(fw.has_target ? fw.target_version : "", target_used ? target_used : "") != 0) return false;
    return manual ? fw.update_request == request : fw.auto_update;
}

static void conclude_request(bool manual, uint32_t request)
{
    if (manual && counter_commit(request, s_last_id) != ESP_OK) {
        ESP_LOGW(TAG, "sync: update request %lu stays open (counter not saved)", (unsigned long)request);
    }
}

static update_result_t run_update(const char *target, bool manual, uint32_t request)
{
    ESP_LOGI(TAG, "sync: firmware %s check target=%s", manual ? "requested" : "auto", target ? target : "latest");
    if (fw_ota_check_update_version(target) != ESP_OK) {
        ESP_LOGW(TAG, "sync: firmware check could not start");
        return UPDATE_DEFERRED;
    }
    fw_ota_update_status_t up;
    if (!wait_update_state(FW_OTA_UPDATE_CHECKING, 90, &up)) {
        ESP_LOGW(TAG, "sync: firmware check timed out");
        conclude_request(manual, request);
        return UPDATE_CONCLUDED;
    }
    s_report_hash = 0;      /* report the result on the next cycle */
    if (up.state != FW_OTA_UPDATE_AVAILABLE) {
        ESP_LOGI(TAG, "sync: firmware check result %s (%s)", update_state_name(up.state), up.detail);
        conclude_request(manual, request);
        return UPDATE_CONCLUDED;
    }
    ESP_LOGI(TAG, "sync: firmware %s available", up.version);
    /* Tell the account before the voice path and the download take over. The
     * answer may carry a new document: it is applied before anything installs. */
    s_report_installing = true;
    due_t due = { .checkin = true };
    (void)run_cycle(esp_timer_get_time(), due);
    s_report_installing = false;
    s_report_hash = 0;
    if (!update_still_wanted(target, manual, request)) {
        ESP_LOGI(TAG, "sync: firmware policy changed during the check, install deferred");
        return UPDATE_DEFERRED;
    }
    /* The check took up to 90 s: the box must still be idle right now. */
    const char *busy = busy_reason();
    if (busy) {
        ESP_LOGI(TAG, "sync: firmware install deferred (%s)", busy);
        return UPDATE_DEFERRED;
    }
    /* The counter is durable before the install can start: a power cut
     * during the download never replays the request. */
    if (manual && counter_commit(request, s_last_id) != ESP_OK) {
        ESP_LOGW(TAG, "sync: firmware install deferred (update counter not saved)");
        return UPDATE_DEFERRED;
    }
    s_report_installing = true;
    ESP_LOGI(TAG, "sync: firmware %s installing", up.version);
    if (fw_ota_install_update() != ESP_OK) {
        s_report_installing = false;
        ESP_LOGW(TAG, "sync: firmware install could not start");
        return UPDATE_CONCLUDED;
    }
    /* The image download ends in a reboot; an error leaves the status for
     * the next check-in. */
    wait_update_state(FW_OTA_UPDATE_AVAILABLE, 30, &up);
    for (int i = 0; i < 1200 && up.state == FW_OTA_UPDATE_INSTALLING; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        fw_ota_get_update_status(&up);
    }
    s_report_installing = false;
    s_report_hash = 0;
    return UPDATE_CONCLUDED;
}

static void firmware_tick(int64_t now)
{
    device_config_firmware_t fw;
    device_config_firmware(&fw);
    if (!fw.valid || !s_have_checkin || now < s_next_update_try_us) return;
    bool manual = fw.update_request > counter_handled();
    bool automatic = !manual && fw.auto_update && now >= s_next_auto_us;
    if (!manual && !automatic) return;
    const char *target = fw.has_target ? fw.target_version : NULL;
    char pinned[sizeof(fw.target_version)];
    if (target) {
        strlcpy(pinned, target, sizeof(pinned));
        target = pinned;
    }
    if (run_update(target, manual, fw.update_request) == UPDATE_DEFERRED) {
        s_next_update_try_us = esp_timer_get_time() + UPDATE_RETRY_US;
    } else if (automatic) {
        s_next_auto_us = esp_timer_get_time() + AUTO_UPDATE_PERIOD_US;
    }
}

/* ------------------------------------------------------------- task */

static bool network_ready(const char **why)
{
    cloud_auth_status_t auth;
    cloud_auth_get_status(&auth);
    if (!auth.paired || auth.pairing) { *why = "not-paired"; return false; }
    if (!net_wifi_is_connected(NULL, 0)) { *why = "no-wifi"; return false; }
    if (time(NULL) < 1704067200LL) { *why = "no-clock"; return false; }
    return true;
}

/* Why the box is busy, or NULL. A prepared idle voice session (warm path)
 * is not busy: only a request in flight, a TLS handshake, playback or a
 * firmware transfer keep the sync off. */
static const char *busy_reason(void)
{
    if (fw_ota_is_busy()) return "firmware";
    if (net_ws_request_active() || net_ws_talk_active() || net_ws_talk_connecting() ||
        net_ws_conversation_active()) return "voice-request";
    if (net_ws_tls_busy()) return "voice-tls";
    if (audio_is_speaking()) return "voice-speaking";
    return NULL;
}

static bool voice_quiet(void)
{
    return busy_reason() == NULL;
}

static void warm_release(void)
{
    if (!s_warm_held) return;
    s_warm_held = false;
    net_ws_warm_hold(false);
}

/* True when the internal RAM for one HTTPS cycle is free. A warm voice
 * session (pre-dialed socket, kept-alive API connection) can hold so much of
 * it that the sync never runs (RLCD 4.2 with its echo canceller): after one
 * poll period of low heap the warm path yields until the cycle is done. */
static bool heap_ready(int64_t now)
{
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (largest >= INTERNAL_BLOCK_MIN && free_int >= internal_free_needed()) {
        s_low_heap_since_us = 0;
        return true;
    }
    if (!s_low_heap_since_us) s_low_heap_since_us = now;
    if (!s_warm_held && now - s_low_heap_since_us >= WARM_YIELD_AFTER_US && now >= s_yield_retry_us) {
        s_warm_held = true;
        s_hold_since_us = now;
        ++s_yields;
        net_ws_warm_hold(true);
        ESP_LOGI(TAG, "sync: internal RAM low for %d s (free %u B, needs %u B; largest %u B), "
                 "the warm voice session yields", (int)((now - s_low_heap_since_us) / 1000000),
                 (unsigned)free_int, (unsigned)internal_free_needed(), (unsigned)largest);
    } else if (s_warm_held && now - s_hold_since_us >= WARM_YIELD_WAIT_US) {
        ESP_LOGW(TAG, "sync: internal RAM still low with the warm session %s (free %u B, needs %u B; "
                 "largest %u B); next try in %d s", net_ws_warm_released() ? "closed" : "open",
                 (unsigned)free_int, (unsigned)internal_free_needed(), (unsigned)largest,
                 (int)(POLL_PERIOD_US / 1000000));
        warm_release();
        s_yield_retry_us = now + POLL_PERIOD_US;
    }
    s_block = s_warm_held ? "warm-yield" : "low-heap";
    return false;
}

static void tick(void)
{
    int64_t now = esp_timer_get_time();
    if (!kbx_board_mismatch()) local_watch(now);
    if (s_warm_held && now - s_hold_since_us >= WARM_HOLD_MAX_US) {
        warm_release();
        s_yield_retry_us = now + POLL_PERIOD_US;
    }
    const char *busy = busy_reason();
    if (busy) {
        s_last_busy_us = now;
        s_block = busy;
        return;
    }
    if (now - s_last_busy_us < QUIET_HOLDOFF_US) { s_block = "holdoff"; return; }
    const char *why = "";
    if (!network_ready(&why)) { s_block = why; return; }
    if (now < s_next_try_us) { s_block = "backoff"; return; }
    s_block = "idle";

    bool poke = s_poke;
    due_t due = {0};
    device_config_state_t st;
    device_config_get_state(&st);
    due.checkin = poke || !s_have_checkin || now - s_last_checkin_us >= CHECKIN_PERIOD_US;
    if (!due.checkin && now - s_last_checkin_us >= CHECKIN_MIN_US) {
        if (!s_hash_buf) s_hash_buf = heap_caps_malloc(HASH_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        due.checkin = s_hash_buf && report_hash(s_hash_buf, HASH_BUF) != s_report_hash;
    }
    due.poll = poke || now - s_last_poll_us >= s_poll_period_us;
    /* A local change needs the account's current revision first. */
    if (st.local_pending && !s_seen_valid) due.poll = true;
    due.write = st.local_pending && now >= s_write_hold_us && s_seen_valid && s_seen_rev == st.applied_revision;
    if (kbx_board_mismatch()) due.poll = due.write = false;   /* check-in only */
    if ((due.checkin || due.poll || due.write) && !heap_ready(now)) return;
    due.bench = s_put_pending;
    if (due.checkin || due.poll || due.write || due.bench) {
        s_poke = false;
        (void)run_cycle(now, due);
        /* A rejected or failed cycle must not retry every second. */
    }
    warm_release();
    if (voice_quiet()) firmware_tick(esp_timer_get_time());
}

static void sync_task(void *arg)
{
    (void)arg;
    s_next_auto_us = esp_timer_get_time() + AUTO_UPDATE_FIRST_US;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        tick();
    }
}

void edge_sync_poke(void)
{
    s_poke = true;
    if (s_task) xTaskNotifyGive(s_task);
}

esp_err_t edge_sync_init(void)
{
    if (s_task) return ESP_OK;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_ctr);
        counter_t stored;
        if (nvs_get_blob(h, NVS_KEY_COUNTER, &stored, &len) == ESP_OK && len == sizeof(stored) &&
            memchr(stored.id, 0, sizeof(stored.id))) {
            s_ctr = stored;
        } else {
            /* v1 stored the pair as two keys: take it over once. */
            size_t n = sizeof(s_ctr.id);
            (void)nvs_get_u32(h, NVS_KEY_HANDLED, &s_ctr.handled);
            if (nvs_get_str(h, NVS_KEY_DEVID, s_ctr.id, &n) != ESP_OK) s_ctr.id[0] = 0;
        }
        nvs_close(h);
    }
    if (xTaskCreateWithCaps(sync_task, "kbx_sync", 16384, NULL, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "sync: task could not start");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sync: gateway %s handled_request=%lu", edge_sync_gateway_base(), (unsigned long)counter_handled());
    return ESP_OK;
}

const char *edge_sync_device_id(void)
{
    /* The id of the last cycle, else the one the update counter saved. */
    if (s_last_id[0]) return s_last_id;
    return s_ctr.id[0] ? s_ctr.id : NULL;
}

void edge_sync_log_status(void)
{
    device_config_state_t st;
    device_config_get_state(&st);
    device_config_firmware_t fw;
    device_config_firmware(&fw);
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "sync: base=%s id=%s block=%s http=%d heap=%u/%u need=%u/%u cost=%u low_heap=%ds yields=%lu",
             edge_sync_gateway_base(), s_last_id[0] ? s_last_id : "-", s_block, s_last_status,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)internal_free_needed(), (unsigned)INTERNAL_BLOCK_MIN, (unsigned)s_cycle_cost,
             s_low_heap_since_us ? (int)((now - s_low_heap_since_us) / 1000000) : 0, (unsigned long)s_yields);
    ESP_LOGI(TAG, "sync: seen=%s%lu applied=%lu pending=%d rejected=%lu %s checkin=%ds poll=%ds/%ds backoff=%ds",
             s_seen_valid ? "" : "?", (unsigned long)s_seen_rev, (unsigned long)st.applied_revision,
             (int)st.local_pending, (unsigned long)s_rejected_rev, s_reject_reason,
             s_have_checkin ? (int)((now - s_last_checkin_us) / 1000000) : -1,
             s_last_poll_us ? (int)((now - s_last_poll_us) / 1000000) : -1,
             (int)(s_poll_period_us / 1000000), s_backoff_s);
    ESP_LOGI(TAG, "sync: firmware target=%s auto_update=%d update_request=%lu handled=%lu next_auto=%ds",
             fw.has_target ? fw.target_version : "latest", (int)fw.auto_update, (unsigned long)fw.update_request,
             (unsigned long)counter_handled(), (int)((s_next_auto_us - now) / 1000000));
}

/* ---------------------------------------------------------- console */

static void log_chunks(const char *label, const char *text)
{
    size_t n = strlen(text);
    for (size_t i = 0; i < n; i += 200) ESP_LOGI(TAG, "%s[%u] %.200s", label, (unsigned)i, text + i);
}

/* KBX sync body: the check-in and write-back bodies this box would send now. */
static void log_bodies(void)
{
    work_t *w = heap_caps_calloc(1, sizeof(work_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w) return;
    bool ok = false;
    if (build_checkin(w, &ok) > 0 && ok) log_chunks("checkin", w->req);
    if (device_config_writeback_json(w->resp, sizeof(w->resp))) log_chunks("settings", w->resp);
    if (device_config_reported_json(w->req, REQ_CAP)) log_chunks("reported", w->req);
    free(w);
}

bool edge_sync_console(const char *line)
{
    while (line && isspace((unsigned char)*line)) line++;
    if (!line) return false;
    if (!strncasecmp(line, "kbx", 3) && isspace((unsigned char)line[3])) {
        line += 4;
        while (isspace((unsigned char)*line)) line++;
    }
    if (strncasecmp(line, "sync", 4) != 0) return false;
    const char *rest = line + 4;
    if (*rest == '?') {
        edge_sync_log_status();
        return true;
    }
    if (*rest && !isspace((unsigned char)*rest)) return false;
    while (isspace((unsigned char)*rest)) rest++;
    if (!*rest || !strcmp(rest, "?")) {
        edge_sync_log_status();
    } else if (!strcasecmp(rest, "body")) {
        log_bodies();
#if CONFIG_KBX_DEV_ENDPOINTS
    } else if (!strncasecmp(rest, "put", 3) && (rest[3] == ' ' || rest[3] == '	')) {
        const char *arg = rest + 3;
        while (isspace((unsigned char)*arg)) arg++;
        char rev[12] = "";
        if ((*arg == 'v' || *arg == 'V') && isdigit((unsigned char)arg[1])) {
            size_t n = 0;
            for (arg++; isdigit((unsigned char)*arg) && n < sizeof(rev) - 1; arg++) rev[n++] = *arg;
            rev[n] = 0;
            while (isspace((unsigned char)*arg)) arg++;
        }
        if (*arg != '{' || strlen(arg) >= sizeof(s_put_body) || s_put_pending) {
            ESP_LOGW(TAG, "sync: need KBX sync put [v<rev>] <settings json> (one at a time)");
        } else {
            strlcpy(s_put_rev, rev, sizeof(s_put_rev));
            strlcpy(s_put_body, arg, sizeof(s_put_body));
            s_put_pending = true;
            edge_sync_poke();
            ESP_LOGI(TAG, "sync: bench put queued");
        }
#endif
    } else if (!strcasecmp(rest, "now")) {
        edge_sync_poke();
        ESP_LOGI(TAG, "sync: check-in and poll requested");
    } else if (!strncasecmp(rest, "base", 4) && (!rest[4] || isspace((unsigned char)rest[4]) || rest[4] == '?')) {
        const char *arg = rest + 4;
        while (isspace((unsigned char)*arg)) arg++;
        if (!*arg || !strcmp(arg, "?")) {
            ESP_LOGI(TAG, "sync: gateway base %s", edge_sync_gateway_base());
        } else {
            ESP_LOGW(TAG, "sync: base is fixed to " EDGE_SYNC_BASE_DEFAULT);
        }
    } else {
        ESP_LOGW(TAG, "sync: need KBX sync? | sync now | sync body | sync base");
    }
    return true;
}
