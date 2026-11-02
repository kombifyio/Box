#include "c6_edge.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "display_design.h"
#include "psa/crypto.h"

#include "c6_audio.h"
#include "c6_voice.h"
#include "c6_wake.h"
#include "net_wifi.h"

static const char *TAG = "c6_edge";

#define BASE_URL    "https://api.kombify.io/v1/connect/edge-devices/"
#define BOARD_ID    C6_BOARD_ID
#define SCHEMA      "speechkit.box_settings.v1"
#define NVS_NS      "c6_edge"
#define ID_MAX      56
#define SUB_MAX     192
#define REQ_CAP     1536
#define US(s)       ((int64_t)(s) * 1000000)
#define BOOT_HOLD_US     US(25)    /* Wi-Fi, clock and token settle first */
#define POLL_DEFAULT_S   60        /* x-kombify-poll-after: 10 while the account UI is open, else 60 */
#define POLL_MIN_S       5
#define POLL_MAX_S       300
#define CHECKIN_PERIOD_US US(300)
#define CHECKIN_MIN_US   US(30)
#define DEBOUNCE_US      US(4)     /* a volume drag is one write-back */
#define BACKOFF_MIN_S    30
#define BACKOFF_MAX_S    300
#define STALE_CONFIRM_US US(180)   /* a lower account revision this long is real */

/* Wake phrase ids as reported; the document's wake.phrase must be one. */
static const char *const PHRASE_ID[C6_WAKE_COUNT] = { "kombify", "hey_kombify", "hey_kubi", "hey_jarvis" };

typedef struct {
    int volume;
    int wake;            /* c6_wake_phrase_t */
    int followups;
    char locale[3];      /* "de" or "en" */
    int design;          /* kbx_design_t: the stored display design */
} local_t;

static char s_id[ID_MAX];
static uint32_t s_applied;           /* account revision the box applied */
static bool s_pending;               /* a local change the account has not seen */
static bool s_have_checkin;
static bool s_seen_valid;
static uint32_t s_seen_rev;
static uint32_t s_rejected;
static char s_reject_reason[24];
static int64_t s_last_checkin_us, s_last_poll_us, s_next_try_us, s_change_us;
static int s_backoff_s;
static uint32_t s_report_hash;
static volatile bool s_poke;
static local_t s_snap;               /* local values at the last sync */
static bool s_snap_valid;
static int s_last_status;
static int s_poll_s = POLL_DEFAULT_S;
static uint32_t s_stale_rev;         /* account revision below s_applied, first seen at: */
static int64_t s_stale_first_us;
static char s_req[REQ_CAP];

/* ── helpers copied from edge_sync.c (its copies are static to the S3 build) ── */

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

/* HMAC-SHA256 on the PSA hash (mbedTLS 4 exposes hashes through PSA). */
static bool hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32])
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
    wipe(pad, sizeof(pad));
    wipe(inner, sizeof(inner));
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
static bool token_subject(const char *token, char *sub, size_t sub_cap)
{
    size_t cap = 0;
    char *claims = c6_voice_scratch(&cap);
    const char *first = strchr(token, '.');
    const char *second = first ? strchr(first + 1, '.') : NULL;
    if (!first || !second || second == first + 1) {
        ESP_LOGW(TAG, "access token is not a JWT");
        return false;
    }
    if (b64url_decode(first + 1, (size_t)(second - first - 1), claims, cap) < 0) {
        ESP_LOGW(TAG, "token payload undecodable (%u chars)", (unsigned)(second - first - 1));
        return false;
    }
    cJSON *root = cJSON_Parse(claims);
    wipe(claims, strlen(claims));
    const cJSON *item = root ? cJSON_GetObjectItemCaseSensitive(root, "sub") : NULL;
    bool ok = false;
    if (cJSON_IsString(item) && item->valuestring[0] && strlen(item->valuestring) < sub_cap) {
        strlcpy(sub, item->valuestring, sub_cap);
        ok = true;
    }
    if (!ok) ESP_LOGW(TAG, "token has no usable sub claim");
    cJSON_Delete(root);
    return ok;
}

/* "dev_" + base64url(HMAC-SHA256("kombify-connect-device:v1:<sub>", key = factory base MAC)). */
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
    wipe(mac, sizeof(mac));
    wipe(mac_hmac, sizeof(mac_hmac));
    return ok;
}

static void sanitize(char *s)
{
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr(" ._:-", *s)) *s = '_';
    }
}

/* ── local values and their sync state ───────────────────────────────── */

static void local_now(local_t *l)
{
    memset(l, 0, sizeof(*l));   /* padding bytes are hashed */
    l->volume = c6_audio_volume();
    l->wake = (int)c6_wake_phrase();
    l->followups = (int)c6_voice_max_followups();
    strlcpy(l->locale, strncmp(c6_voice_locale(), "de", 2) == 0 ? "de" : "en", sizeof(l->locale));
    l->design = (int)kbx_design_stored();
}

static bool local_differs(const local_t *a, const local_t *b)
{
    return a->volume != b->volume || a->wake != b->wake || a->followups != b->followups ||
           strcmp(a->locale, b->locale) != 0 || a->design != b->design;
}

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "rev", s_applied);
    nvs_set_u8(h, "pend", s_pending ? 1 : 0);
    nvs_set_str(h, "id", s_id);
    nvs_commit(h);
    nvs_close(h);
}

static void load_state(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t pend = 0;
    size_t n = sizeof(s_id);
    nvs_get_u32(h, "rev", &s_applied);
    nvs_get_u8(h, "pend", &pend);
    if (nvs_get_str(h, "id", s_id, &n) != ESP_OK) s_id[0] = 0;
    s_pending = pend != 0;
    nvs_close(h);
}

/* Volume, wake phrase, follow-ups and language have no change hook: notice
 * them here. A change settles for 4 s so a slider drag is one write-back. */
static void local_watch(int64_t now)
{
    local_t cur;
    local_now(&cur);
    if (!s_snap_valid) {
        s_snap = cur;
        s_snap_valid = true;
        return;
    }
    if (!local_differs(&cur, &s_snap)) {
        s_change_us = 0;
        return;
    }
    static local_t cand;
    if (!s_change_us || local_differs(&cur, &cand)) {
        cand = cur;
        s_change_us = now;
        return;
    }
    if (now - s_change_us >= DEBOUNCE_US) {
        s_snap = cur;
        s_change_us = 0;
        if (!s_pending) {
            s_pending = true;
            persist();
            ESP_LOGI(TAG, "local change pending for the account");
        }
    }
}

/* ── JSON bodies ─────────────────────────────────────────────────────── */

static size_t reported_body(char *out, size_t cap)
{
    local_t l;
    local_now(&l);
    int n = snprintf(out, cap,
                     "{\"wake\":{\"enabled\":%s,\"ready\":%s,\"phrase\":\"%s\",\"phrases\":[\"%s\",\"%s\",\"%s\",\"%s\"]},"
                     "\"assist\":{\"max_followups\":%d},\"audio\":{\"volume\":%d},\"locale\":\"%s\","
                     "\"locales\":[\"de\",\"en\"],\"display\":{\"design\":\"%s\",\"designs\":[\"classic\",\"watch\"]},"
                     "\"supports\":[\"wake.enabled\",\"wake.phrase\",\"assist.max_followups\",\"audio.volume\",\"locale\","
                     "\"display.design\"]}",
                     l.wake >= 0 ? "true" : "false", c6_wake_listening() ? "true" : "false",
                     l.wake >= 0 ? PHRASE_ID[l.wake] : PHRASE_ID[C6_WAKE_HEY_JARVIS], PHRASE_ID[0], PHRASE_ID[1],
                     PHRASE_ID[2], PHRASE_ID[3], l.followups, l.volume, l.locale,
                     kbx_design_id((kbx_design_t)l.design));
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static uint32_t local_hash(void)
{
    local_t l;
    local_now(&l);
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)&l;
    for (size_t i = 0; i < sizeof(l); i++) h = (h ^ p[i]) * 16777619u;
    uint32_t extra[3] = { s_applied, s_pending, s_rejected };
    p = (const unsigned char *)extra;
    for (size_t i = 0; i < sizeof(extra); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static bool build_checkin(void)
{
    char rep[768];
    char ip[16] = "";
    net_wifi_is_connected(ip, sizeof(ip));
    if (!reported_body(rep, sizeof(rep))) return false;
    char sync[128];
    int sn = snprintf(sync, sizeof(sync), "{\"applied_revision\":%lu,\"local_pending\":%s", (unsigned long)s_applied,
                      s_pending ? "true" : "false");
    if (s_rejected && sn > 0 && (size_t)sn < sizeof(sync))
        sn += snprintf(sync + sn, sizeof(sync) - (size_t)sn, ",\"rejected_revision\":%lu,\"reject_reason\":\"%s\"",
                       (unsigned long)s_rejected, s_reject_reason);
    if (sn <= 0 || (size_t)sn >= sizeof(sync) - 1) return false;
    sync[sn++] = '}';
    sync[sn] = 0;
    int n = snprintf(s_req, sizeof(s_req),
                     "{\"schema\":\"kombify.edge_device.checkin.v1\",\"kind\":\"kombify-box\",\"board\":\"" BOARD_ID
                     "\",\"firmware_version\":\"%s\",\"display_name\":\"kombify Box C6\"%s%s%s,\"reported\":%s,"
                     "\"sync\":%s,\"firmware_update\":{\"state\":\"idle\",\"detail\":\"\",\"progress_percent\":0,"
                     "\"handled_request\":0}}",
                     esp_app_get_description()->version, ip[0] ? ",\"local_ip\":\"" : "", ip, ip[0] ? "\"" : "", rep,
                     sync);
    return n > 0 && (size_t)n < sizeof(s_req);
}

/* The local values as a settings document (write-back and seed). */
static bool build_writeback(void)
{
    local_t l;
    local_now(&l);
    int n = snprintf(s_req, sizeof(s_req),
                     "{\"schema\":\"" SCHEMA "\",\"wake\":{\"enabled\":%s,\"phrase\":\"%s\"},"
                     "\"assist\":{\"max_followups\":%d},\"audio\":{\"volume\":%d},\"locale\":\"%s\"%s%s%s}",
                     l.wake >= 0 ? "true" : "false", PHRASE_ID[l.wake >= 0 ? l.wake : C6_WAKE_HEY_JARVIS],
                     l.followups, l.volume, l.locale,
                     kbx_design_chosen() ? ",\"display\":{\"design\":\"" : "",
                     kbx_design_chosen() ? kbx_design_id((kbx_design_t)l.design) : "",
                     kbx_design_chosen() ? "\"}" : "");
    return n > 0 && (size_t)n < sizeof(s_req);
}

/* ── the settings document ───────────────────────────────────────────── */

typedef enum { APPLIED, CURRENT, STALE, REJECTED } outcome_t;

static bool whole(const cJSON *v, double max)
{
    return cJSON_IsNumber(v) && v->valuedouble >= 0.0 && v->valuedouble <= max && v->valuedouble == (double)(uint32_t)v->valuedouble;
}

/* Validates the members the C6 has; every other member is ignored. */
static outcome_t apply_document(const char *json, size_t len, uint32_t *revision, const char **reason)
{
    *reason = NULL;
    *revision = 0;
    cJSON *root = cJSON_ParseWithLength(json, len);
    outcome_t out = REJECTED;
    if (!cJSON_IsObject(root)) {
        *reason = "json";
        goto done;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(root, "revision");
    if (!cJSON_IsString(schema) || strcmp(schema->valuestring, SCHEMA)) {
        *reason = "schema";
        goto done;
    }
    if (!cJSON_IsNumber(rev) || rev->valuedouble < 1.0 || rev->valuedouble > 2147483647.0 ||
        rev->valuedouble != (double)(uint32_t)rev->valuedouble) {
        *reason = "revision";
        goto done;
    }
    *revision = (uint32_t)rev->valuedouble;
    const cJSON *wake = cJSON_GetObjectItemCaseSensitive(root, "wake");
    const cJSON *assist = cJSON_GetObjectItemCaseSensitive(root, "assist");
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    const cJSON *enabled = wake ? cJSON_GetObjectItemCaseSensitive(wake, "enabled") : NULL;
    const cJSON *phrase = wake ? cJSON_GetObjectItemCaseSensitive(wake, "phrase") : NULL;
    const cJSON *mf = assist ? cJSON_GetObjectItemCaseSensitive(assist, "max_followups") : NULL;
    const cJSON *vol = audio ? cJSON_GetObjectItemCaseSensitive(audio, "volume") : NULL;
    const cJSON *locale = cJSON_GetObjectItemCaseSensitive(root, "locale");
    const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    const cJSON *dsg = cJSON_IsObject(display) ? cJSON_GetObjectItemCaseSensitive(display, "design") : NULL;
    kbx_design_t design = KBX_DESIGN_WATCH;
    int phrase_idx = -1;
    for (int i = 0; phrase && cJSON_IsString(phrase) && i < C6_WAKE_COUNT; i++)
        if (!strcmp(phrase->valuestring, PHRASE_ID[i])) phrase_idx = i;
    if (enabled && !cJSON_IsBool(enabled)) *reason = "wake.enabled";
    else if (phrase && phrase_idx < 0) *reason = "wake.phrase";
    else if (mf && !whole(mf, 3.0)) *reason = "assist.max_followups";
    else if (vol && !whole(vol, 100.0)) *reason = "audio.volume";
    else if (locale && (!cJSON_IsString(locale) || (strcmp(locale->valuestring, "de") && strcmp(locale->valuestring, "en"))))
        *reason = "locale";
    else if (display && !cJSON_IsObject(display)) *reason = "display";
    else if (dsg && (!cJSON_IsString(dsg) || !kbx_design_from_id(dsg->valuestring, &design))) *reason = "display.design";
    if (*reason) goto done;

    if (*revision < s_applied) {
        out = STALE;
        goto done;
    }
    if (*revision == s_applied) {
        out = CURRENT;
        goto done;
    }
    /* Phrase before enabled, so enabling picks the selected model. */
    if (cJSON_IsFalse(enabled)) {
        c6_wake_set_phrase(C6_WAKE_OFF);
    } else if (phrase_idx >= 0) {
        if (c6_wake_phrase() != (c6_wake_phrase_t)phrase_idx) c6_wake_set_phrase((c6_wake_phrase_t)phrase_idx);
    } else if (cJSON_IsTrue(enabled) && c6_wake_phrase() == C6_WAKE_OFF) {
        c6_wake_set_phrase(C6_WAKE_HEY_JARVIS);
    }
    if (mf && c6_voice_max_followups() != (unsigned)mf->valuedouble) c6_voice_set_max_followups((unsigned)mf->valuedouble);
    if (vol && (unsigned)c6_audio_volume() != (unsigned)vol->valuedouble) c6_audio_set_volume((int)vol->valuedouble);
    if (locale) c6_voice_set_locale(!strcmp(locale->valuestring, "de") ? "de-DE" : "en-US");
    /* The screen is built once at start: the choice is stored and the UI
     * timer restarts the box into it when it is idle (c6_ui.c). */
    if (dsg && (kbx_design_stored() != design || !kbx_design_chosen())) (void)kbx_design_set(design);
    out = APPLIED;
    ESP_LOGI(TAG, "applied revision %lu: wake=%s followups=%s volume=%s locale=%s design=%s", (unsigned long)*revision,
             wake ? (cJSON_IsFalse(enabled) ? "off" : phrase ? phrase->valuestring : "on") : "-",
             mf ? "set" : "-", vol ? "set" : "-", locale ? locale->valuestring : "-",
             dsg ? kbx_design_id(design) : "-");
done:
    cJSON_Delete(root);
    return out;
}

/* The account serves a revision below the one this box applied. Once is a
 * lagging read and is ignored; the same revision STALE_CONFIRM_US later means
 * the account's numbering restarted (or a bench apply ran ahead of it). Left
 * alone the box waits forever: no newer document comes, and a pending local
 * change is written only on the applied revision (seen 9, applied 11). */
static bool stale_confirmed(uint32_t revision)
{
    int64_t now = esp_timer_get_time();
    if (s_stale_rev != revision) {
        s_stale_rev = revision;
        s_stale_first_us = now;
        ESP_LOGI(TAG, "account serves revision %lu below applied %lu; adopted if it stays %d s",
                 (unsigned long)revision, (unsigned long)s_applied, (int)(STALE_CONFIRM_US / 1000000));
        return false;
    }
    if (now - s_stale_first_us < STALE_CONFIRM_US) return false;
    s_stale_rev = 0;
    return true;
}

/* Take the account's numbering. A pending local change is written on the
 * account's revision (If-Match); without one the applied revision drops below
 * it so the account's document applies. Returns whether a change is pending. */
static bool rebase(uint32_t revision)
{
    ESP_LOGI(TAG, "account at revision %lu below applied %lu: %s", (unsigned long)revision,
             (unsigned long)s_applied, s_pending ? "the local change is written on it" : "its document applies");
    s_applied = s_pending ? revision : revision - 1;
    persist();
    return s_pending;
}

/* One account document (also the revision 0 "unset" one). True: the account
 * has none and the box seeds it from its own values. */
static bool take_settings(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *rev = root ? cJSON_GetObjectItemCaseSensitive(root, "revision") : NULL;
    bool zero = cJSON_IsNumber(rev) && rev->valuedouble == 0.0;
    cJSON_Delete(root);
    if (zero) {
        s_seen_valid = true;
        s_seen_rev = 0;
        ESP_LOGI(TAG, "account has no settings document (revision 0), seeding from this box");
        return true;
    }
    const char *reason = NULL;
    uint32_t revision = 0;
    outcome_t out = apply_document(json, len, &revision, &reason);
    if (out == STALE && revision && stale_confirmed(revision) && !rebase(revision))
        out = apply_document(json, len, &revision, &reason);
    if (out == APPLIED || out == CURRENT) s_stale_rev = 0;
    if (revision) {
        s_seen_valid = true;
        s_seen_rev = revision;
    }
    if (out == REJECTED) {
        s_rejected = revision;
        strlcpy(s_reject_reason, reason ? reason : "invalid", sizeof(s_reject_reason));
        sanitize(s_reject_reason);
        ESP_LOGW(TAG, "revision %lu rejected (%s), nothing applied", (unsigned long)revision, s_reject_reason);
    } else if (out == APPLIED || out == CURRENT) {
        s_rejected = 0;
        s_reject_reason[0] = 0;
    }
    if (out == APPLIED) {
        /* The account's document replaces any unsent local change. */
        s_applied = revision;
        s_pending = false;
        local_now(&s_snap);
        s_snap_valid = true;
        persist();
    }
    return false;
}

/* ── requests ────────────────────────────────────────────────────────── */

static int gw_call(const char *method, const char *suffix, const char *extra, const char *body,
                   const char **resp, const char **etag)
{
    char url[sizeof(BASE_URL) + ID_MAX + 12];
    snprintf(url, sizeof(url), BASE_URL "%s%s", s_id, suffix);
    int poll_after = 0;
    s_last_status = c6_voice_gateway_call(method, url, extra, body, resp, etag, &poll_after);
    /* The Gateway tells how soon the account UI wants the next poll. */
    if (s_last_status >= 200 && s_last_status < 400 && poll_after > 0) {
        int clamped = poll_after < POLL_MIN_S ? POLL_MIN_S : poll_after > POLL_MAX_S ? POLL_MAX_S : poll_after;
        if (clamped != s_poll_s) ESP_LOGI(TAG, "poll period %d s (Gateway hint %d s)", clamped, poll_after);
        s_poll_s = clamped;
    }
    return s_last_status;
}

static void log_denial(const char *what, const char *resp)
{
    char code[48] = "";
    cJSON *root = resp[0] ? cJSON_Parse(resp) : NULL;
    const cJSON *rc = root ? cJSON_GetObjectItemCaseSensitive(root, "reason_code") : NULL;
    if (cJSON_IsString(rc)) strlcpy(code, rc->valuestring, sizeof(code));
    cJSON_Delete(root);
    sanitize(code);
    ESP_LOGW(TAG, "%s refused http=%d reason=%s", what, s_last_status, code);
}

static bool do_checkin(bool *seed)
{
    if (!build_checkin()) {
        ESP_LOGW(TAG, "check-in body unavailable");
        return false;
    }
    uint32_t hash = local_hash();
    const char *resp, *etag;
    int status = gw_call("PUT", "", NULL, s_req, &resp, &etag);
    if (status < 200 || status > 299) {
        if (status) log_denial("check-in", resp);
        else ESP_LOGW(TAG, "check-in failed (transport)");
        return false;
    }
    s_have_checkin = true;
    s_last_checkin_us = s_last_poll_us = esp_timer_get_time();
    s_report_hash = hash;
    ESP_LOGI(TAG, "check-in http=%d applied=%lu pending=%d", status, (unsigned long)s_applied, (int)s_pending);
    cJSON *root = cJSON_Parse(resp);
    const cJSON *settings = root ? cJSON_GetObjectItemCaseSensitive(root, "settings") : NULL;
    if (cJSON_IsObject(settings)) {
        char *doc = cJSON_PrintUnformatted(settings);
        if (doc) {
            *seed = take_settings(doc, strlen(doc));
            free(doc);
        }
    }
    cJSON_Delete(root);
    return true;
}

static bool do_poll(bool *seed, bool *need_checkin)
{
    char extra[48] = "";
    if (s_seen_valid) snprintf(extra, sizeof(extra), "If-None-Match: \"v%lu\"\r\n", (unsigned long)s_seen_rev);
    const char *resp, *etag;
    int status = gw_call("GET", "/settings", extra[0] ? extra : NULL, NULL, &resp, &etag);
    s_last_poll_us = esp_timer_get_time();
    if (status == 304) {
        ESP_LOGI(TAG, "settings http=304 (revision %lu)", (unsigned long)s_seen_rev);
        if (s_seen_valid && s_seen_rev && s_seen_rev < s_applied && stale_confirmed(s_seen_rev) &&
            !rebase(s_seen_rev)) {
            s_seen_valid = false;      /* fetch the document to apply it */
            s_last_poll_us = 0;
        }
        return true;
    }
    if (status == 404) {
        s_have_checkin = false;     /* Gateway has no record yet */
        *need_checkin = true;
        ESP_LOGI(TAG, "settings http=404, checking in");
        return true;
    }
    if (status < 200 || status > 299) {
        if (status) log_denial("settings", resp);
        else ESP_LOGW(TAG, "settings poll failed (transport)");
        return false;
    }
    ESP_LOGI(TAG, "settings http=%d", status);
    *seed = take_settings(resp, strlen(resp));
    return true;
}

/* PUT the local values with If-Match: a pending local change or the seed of
 * a revision 0 account. */
static bool do_write(bool seed)
{
    if (!build_writeback()) return false;
    local_t at;
    local_now(&at);
    char extra[48];
    snprintf(extra, sizeof(extra), "If-Match: \"v%lu\"\r\n",
             (unsigned long)(seed ? 0 : s_applied));
    const char *resp, *etag;
    int status = gw_call("PUT", "/settings", extra, s_req, &resp, &etag);
    if (status == 412) {
        ESP_LOGI(TAG, "write-back http=412, the account moved on; its newer document wins");
        s_seen_valid = false;
        s_last_poll_us = 0;
        return true;
    }
    if (status == 400) {
        log_denial("write-back", resp);
        s_pending = false;         /* the account refuses these values: do not loop */
        persist();
        return true;
    }
    if (status < 200 || status > 299) {
        if (status) log_denial("write-back", resp);
        return false;
    }
    uint32_t revision = 0;
    const char *p = strchr(etag, 'v');
    if (p) revision = (uint32_t)strtoul(p + 1, NULL, 10);
    if (!revision) {
        cJSON *root = cJSON_Parse(resp);
        const cJSON *r = root ? cJSON_GetObjectItemCaseSensitive(root, "revision") : NULL;
        if (cJSON_IsNumber(r) && r->valuedouble >= 1) revision = (uint32_t)r->valuedouble;
        cJSON_Delete(root);
    }
    if (!revision) {
        ESP_LOGW(TAG, "write-back http=%d without a revision", status);
        s_seen_valid = false;
        return true;
    }
    s_seen_valid = true;
    s_seen_rev = s_applied = revision;
    local_t now_vals;
    local_now(&now_vals);
    /* A local edit after the body was built stays pending. */
    s_pending = local_differs(&at, &now_vals);
    s_snap = at;
    persist();
    ESP_LOGI(TAG, "%s http=%d account at revision %lu", seed ? "seed" : "write-back", status, (unsigned long)revision);
    return true;
}

/* ── schedule ────────────────────────────────────────────────────────── */

typedef struct {
    bool checkin, poll, write;
} due_t;

static due_t compute_due(void)
{
    due_t d = { 0 };
    int64_t now = esp_timer_get_time();
    local_watch(now);
    if (now < BOOT_HOLD_US || now < s_next_try_us) return d;
    d.checkin = s_poke || !s_have_checkin || now - s_last_checkin_us >= CHECKIN_PERIOD_US;
    if (!d.checkin && now - s_last_checkin_us >= CHECKIN_MIN_US) d.checkin = local_hash() != s_report_hash;
    d.poll = !d.checkin && now - s_last_poll_us >= US(s_poll_s);
    d.write = s_pending && s_seen_valid && s_seen_rev == s_applied;
    return d;
}

void c6_edge_init(void)
{
    kbx_design_init(KBX_DESIGN_WATCH);   /* the C6 defaults to the watch design */
    load_state();
    local_now(&s_snap);
    s_snap_valid = true;
    ESP_LOGI(TAG, "edge sync: applied revision %lu, local change %s", (unsigned long)s_applied,
             s_pending ? "pending" : "none");
}

bool c6_edge_due(void)
{
    due_t d = compute_due();
    return d.checkin || d.poll || d.write;
}

void c6_edge_poke(void)
{
    s_poke = true;
    s_next_try_us = 0;
}

void c6_edge_run(const char *bearer)
{
    due_t due = compute_due();
    if (!due.checkin && !due.poll && !due.write) return;
    int64_t t0 = esp_timer_get_time();
    char sub[SUB_MAX], id[ID_MAX];
    bool ok = token_subject(bearer, sub, sizeof(sub)) && derive_device_id(sub, id, sizeof(id));
    wipe(sub, sizeof(sub));
    if (!ok) {
        ESP_LOGW(TAG, "device id unavailable");
    } else if (strcmp(id, s_id)) {
        /* Another account (or the first one): its revisions are not ours. */
        ESP_LOGI(TAG, "device %s", id);
        strlcpy(s_id, id, sizeof(s_id));
        s_applied = 0;
        s_pending = false;
        s_have_checkin = false;
        s_seen_valid = false;
        s_rejected = 0;
        s_stale_rev = 0;
        persist();
        due.checkin = true;
    }
    bool seed = false, need_checkin = false;
    s_poke = false;
    if (ok && due.checkin) ok = do_checkin(&seed);
    else if (ok && due.poll) ok = do_poll(&seed, &need_checkin);
    if (ok && need_checkin) ok = do_checkin(&seed);
    /* Seed an account without a document; write a local change only on the
     * revision the box applied, otherwise the account's newer document wins. */
    if (ok && (seed || (s_pending && s_seen_valid && s_seen_rev == s_applied))) ok = do_write(seed);
    if (ok) {
        s_backoff_s = 0;
        s_next_try_us = 0;
    } else {
        s_backoff_s = s_backoff_s ? s_backoff_s * 2 : BACKOFF_MIN_S;
        if (s_backoff_s > BACKOFF_MAX_S) s_backoff_s = BACKOFF_MAX_S;
        s_next_try_us = esp_timer_get_time() + US(s_backoff_s);
        ESP_LOGW(TAG, "retry in %d s", s_backoff_s);
    }
    ESP_LOGI(TAG, "cycle %s in %d ms (checkin=%d poll=%d)", ok ? "ok" : "failed",
             (int)((esp_timer_get_time() - t0) / 1000), (int)due.checkin, (int)due.poll);
}

unsigned c6_edge_wait_ms(unsigned max_ms)
{
    int64_t next = s_last_poll_us + US(s_poll_s);
    int64_t ms = (next - esp_timer_get_time()) / 1000;
    if (ms < 1000) ms = 1000;
    return ms < (int64_t)max_ms ? (unsigned)ms : max_ms;
}

void c6_edge_log_status(void)
{
    ESP_LOGI(TAG, "edge: device=%s applied=%lu seen=%s%lu pending=%d rejected=%lu last_http=%d checkin=%s poll=%ds "
             "stale=%lu/%ds",
             s_id[0] ? s_id : "-", (unsigned long)s_applied, s_seen_valid ? "" : "?", (unsigned long)s_seen_rev,
             (int)s_pending, (unsigned long)s_rejected, s_last_status, s_have_checkin ? "yes" : "no", s_poll_s,
             (unsigned long)s_stale_rev,
             s_stale_rev ? (int)((esp_timer_get_time() - s_stale_first_us) / 1000000) : 0);
}
