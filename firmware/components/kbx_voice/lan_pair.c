/*
 * kombify box - lan_pair: DNS-SD discovery of a homelab speechkit-server and
 * LAN device pairing with a per-device token (see lan_pair.h).
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "mdns.h"
#include "nvs.h"

#include "cloud_auth.h"
#include "lan_pair.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "secure_zero.h"
#include "session_url.h"

static const char *TAG = "lan_pair";

#define LAN_URL_MAX 200
#define LAN_RESP_MAX 2048
#define LAN_TOKEN_MAX 160
#define LAN_SECRET_MAX 96
#define LAN_WIFI_WAIT_S 90
#define LAN_MDNS_TIMEOUT_MS 3000
#define LAN_MDNS_ATTEMPTS 3
#define LAN_MDNS_MAX_RESULTS 8
#define LAN_HTTP_TIMEOUT_MS 8000
#define LAN_DEVICE_TOKEN_PREFIX "skd_"

typedef struct {
    char url[LAN_URL_MAX + 1];
    char instance[64];
    char call_url[LAN_URL_MAX + 128];
    char body[256];
    char resp[LAN_RESP_MAX];
    char request_id[64];
    char secret[LAN_SECRET_MAX];
    char token[LAN_TOKEN_MAX];
} lan_work_t;

static SemaphoreHandle_t s_lock;
static lan_pair_status_t s_status;
static uint32_t s_generation;
static bool s_running;
static bool s_queued;
static bool s_q_boot, s_q_discover, s_q_pair;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static bool current(uint32_t gen)
{
    lock();
    bool ok = s_generation == gen;
    unlock();
    return ok;
}

static void set_phase(uint32_t gen, lan_pair_phase_t phase, const char *error)
{
    lock();
    if (s_generation == gen) {
        s_status.phase = phase;
        strlcpy(s_status.error, error ? error : "", sizeof(s_status.error));
        if (phase != LAN_PAIR_SHOW_CODE) {
            s_status.code[0] = '\0';
            s_status.expires_in = 0;
        }
        s_status.seq++;
    }
    unlock();
}

static void fail(uint32_t gen, const char *reason)
{
    ESP_LOGW(TAG, "pairing failed: %s", reason);
    set_phase(gen, LAN_PAIR_FAILED, reason);
}

static void set_server(uint32_t gen, const char *url, const char *instance)
{
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    lock();
    if (s_generation == gen) {
        size_t n = strcspn(host, "/");
        if (n >= sizeof(s_status.server)) n = sizeof(s_status.server) - 1;
        memcpy(s_status.server, host, n);
        s_status.server[n] = '\0';
        strlcpy(s_status.instance, instance ? instance : "", sizeof(s_status.instance));
        s_status.seq++;
    }
    unlock();
}

/* Sleep in short steps so a cancel ends the wait at once. */
static bool wait_current(uint32_t gen, int ms)
{
    for (int waited = 0; waited < ms; waited += 250) {
        if (!current(gen)) return false;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return current(gen);
}

/* ── NVS view of net_ws provisioning (read only; net_ws owns the writes) ── */

static void stored_provisioning(char *url, size_t url_cap, bool *token_set)
{
    url[0] = '\0';
    *token_set = false;
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = url_cap;
    if (nvs_get_str(h, "sk_url", url, &n) != ESP_OK) url[0] = '\0';
    n = 0;
    *token_set = nvs_get_str(h, "sk_token", NULL, &n) == ESP_OK && n > 1;
    nvs_close(h);
}

/* ── DNS-SD ─────────────────────────────────────────────────────────────── */

static const char *txt_get(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcasecmp(r->txt[i].key, key) == 0) {
            return r->txt[i].value ? r->txt[i].value : "";
        }
    }
    return NULL;
}

/* Same rule as the server's discovery.ParseTXT: an announcement that
 * carries credential keys is rejected whole. */
static bool txt_has_credential(const mdns_result_t *r)
{
    static const char *const keys[] = { "token", "auth", "password", "secret", "bearer" };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        if (txt_get(r, keys[k])) return true;
    }
    return false;
}

static bool modes_include_voiceagent(const char *modes)
{
    if (!modes) return false;
    const char *p = modes;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        size_t n = strcspn(p, ",");
        size_t m = n;
        while (m > 0 && isspace((unsigned char)p[m - 1])) m--;
        if (m == strlen("voiceagent") && strncasecmp(p, "voiceagent", m) == 0) return true;
        p += n;
    }
    return false;
}

static bool url_acceptable(const char *url)
{
    size_t n = strlen(url);
    if (n < 10 || n > LAN_URL_MAX) return false;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= ' ' || c == '"' || c == '\\' || c >= 0x7f) return false;
    }
    return true;
}

/* Pick the announced server: modes must include voiceagent; one that
 * announces open device pairing (pair=1) wins over one that does not. */
static bool discover(uint32_t gen, lan_work_t *w)
{
    bool inited = false;
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        inited = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns init: %s", esp_err_to_name(err));
        return false;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) (void)mdns_netif_action(sta, MDNS_EVENT_ENABLE_IP4);

    bool found = false;
    int best = -1;
    for (int attempt = 0; attempt < LAN_MDNS_ATTEMPTS && !found && current(gen); attempt++) {
        mdns_result_t *results = NULL;
        err = mdns_query_ptr("_speechkit", "_tcp", LAN_MDNS_TIMEOUT_MS, LAN_MDNS_MAX_RESULTS, &results);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mdns query: %s", esp_err_to_name(err));
        }
        for (mdns_result_t *r = results; r; r = r->next) {
            const char *name = r->instance_name ? r->instance_name : "?";
            mdns_result_t *txt_result = NULL;
            const mdns_result_t *view = r;
            if (r->txt_count == 0 && r->instance_name) {
                /* PTR answer without TXT: ask for it explicitly. */
                if (mdns_query_txt(r->instance_name, "_speechkit", "_tcp", 1500, &txt_result) == ESP_OK && txt_result) {
                    view = txt_result;
                }
            }
            const char *url = txt_get(view, "url");
            const char *modes = txt_get(view, "modes");
            const char *pair = txt_get(view, "pair");
            int score = -1;
            if (txt_has_credential(view)) {
                ESP_LOGW(TAG, "discover: %s ignored (credential in TXT)", name);
            } else if (!url || !url_acceptable(url)) {
                ESP_LOGW(TAG, "discover: %s ignored (no usable url)", name);
            } else if (!modes_include_voiceagent(modes)) {
                ESP_LOGI(TAG, "discover: %s ignored (modes=%s)", name, modes ? modes : "-");
            } else {
                score = (pair && strcmp(pair, "1") == 0) ? 2 : 1;
                ESP_LOGI(TAG, "discover: %s url=%s pair=%s", name, url, pair ? pair : "-");
            }
            if (score > best) {
                best = score;
                strlcpy(w->url, url, sizeof(w->url));
                strlcpy(w->instance, name, sizeof(w->instance));
                found = true;
            }
            if (txt_result) mdns_query_results_free(txt_result);
        }
        if (results) mdns_query_results_free(results);
        if (!found && attempt + 1 < LAN_MDNS_ATTEMPTS && !wait_current(gen, 2000)) break;
    }
    if (inited) mdns_free();
    if (found) {
        size_t n = strlen(w->url);
        while (n > 0 && w->url[n - 1] == '/') w->url[--n] = '\0';
    }
    return found && current(gen);
}

/* ── HTTP ───────────────────────────────────────────────────────────────── */

static esp_err_t http_call(lan_work_t *w, esp_http_client_method_t method, const char *body,
                           const char *secret, int *status)
{
    *status = 0;
    w->resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = w->call_url,
        .method = method,
        .timeout_ms = LAN_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(h, "Accept", "application/json");
    if (body) esp_http_client_set_header(h, "Content-Type", "application/json");
    if (secret) esp_http_client_set_header(h, "X-Pairing-Secret", secret);
    int len = body ? (int)strlen(body) : 0;
    esp_err_t err = esp_http_client_open(h, len);
    if (err == ESP_OK && len > 0 && esp_http_client_write(h, body, len) != len) err = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_fetch_headers(h) < 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(h);
        int total = 0;
        for (;;) {
            int n = esp_http_client_read(h, w->resp + total, (int)sizeof(w->resp) - 1 - total);
            if (n <= 0) break;
            total += n;
            if (total >= (int)sizeof(w->resp) - 1) break;
        }
        w->resp[total] = '\0';
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return err;
}

static bool json_string(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= cap) return false;
    strlcpy(out, v->valuestring, cap);
    return true;
}

static int json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static const char *refusal_reason(const char *resp)
{
    cJSON *root = cJSON_Parse(resp);
    const char *reason = "refused";
    const cJSON *error = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
    const cJSON *code = error ? cJSON_GetObjectItemCaseSensitive(error, "code") : NULL;
    if (cJSON_IsString(code) && code->valuestring) {
        if (!strcmp(code->valuestring, "device_pairing_disabled")) reason = "pairing_off";
        else if (!strcmp(code->valuestring, "not_local_network")) reason = "not_lan";
    }
    cJSON_Delete(root);
    return reason;
}

static bool token_acceptable(const char *token)
{
    size_t n = strlen(token);
    if (n < strlen(LAN_DEVICE_TOKEN_PREFIX) + 32 || n >= LAN_TOKEN_MAX) return false;
    if (strncmp(token, LAN_DEVICE_TOKEN_PREFIX, strlen(LAN_DEVICE_TOKEN_PREFIX)) != 0) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)token[i]) && token[i] != '_' && token[i] != '-') return false;
    }
    return true;
}

/* ── Pairing ────────────────────────────────────────────────────────────── */

static void pair_with(uint32_t gen, lan_work_t *w)
{
    set_phase(gen, LAN_PAIR_REQUESTING, NULL);
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(w->body, sizeof(w->body),
             "{\"device_name\":\"kombify Box %02X%02X\",\"device_kind\":\"box\","
             "\"device_id\":\"kbx-%02x%02x%02x%02x%02x%02x\"}",
             mac[4], mac[5], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(w->call_url, sizeof(w->call_url), "%s/v1/pairing/device-requests", w->url);
    int status = 0;
    if (http_call(w, HTTP_METHOD_POST, w->body, NULL, &status) != ESP_OK) {
        fail(gen, "unreachable");
        return;
    }
    if (status == 403) { fail(gen, refusal_reason(w->resp)); return; }
    if (status == 404) { fail(gen, "no_pairing"); return; }
    if (status == 429) { fail(gen, "busy"); return; }
    if (status != 201) {
        ESP_LOGW(TAG, "pairing request http=%d", status);
        fail(gen, "server_error");
        return;
    }
    cJSON *root = cJSON_Parse(w->resp);
    char code[8] = "";
    bool ok = root && json_string(root, "request_id", w->request_id, sizeof(w->request_id)) &&
              json_string(root, "poll_secret", w->secret, sizeof(w->secret)) &&
              json_string(root, "code", code, sizeof(code));
    int expires = root ? json_int(root, "expires_in", 600) : 0;
    int interval = root ? json_int(root, "poll_interval", 3) : 3;
    cJSON_Delete(root);
    for (size_t i = 0; ok && code[i]; i++) ok = isdigit((unsigned char)code[i]) != 0;
    if (!ok || strlen(code) != 6 || strspn(w->request_id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(w->request_id)) {
        fail(gen, "invalid_response");
        return;
    }
    if (expires <= 0 || expires > 3600) expires = 600;
    if (interval < 2) interval = 2;
    if (interval > 10) interval = 10;
    int64_t deadline = esp_timer_get_time() + (int64_t)expires * 1000000;
    lock();
    if (s_generation == gen) {
        s_status.phase = LAN_PAIR_SHOW_CODE;
        strlcpy(s_status.code, code, sizeof(s_status.code));
        s_status.expires_in = expires;
        s_status.error[0] = '\0';
        s_status.seq++;
    }
    unlock();
    ESP_LOGI(TAG, "code %s on %s - approve on the server: speechkit-server pairing approve %s",
             code, w->url, code);

    snprintf(w->call_url, sizeof(w->call_url), "%s/v1/pairing/device-requests/%s", w->url, w->request_id);
    while (esp_timer_get_time() < deadline) {
        if (!wait_current(gen, interval * 1000)) return;
        if (http_call(w, HTTP_METHOD_GET, NULL, w->secret, &status) != ESP_OK) continue;
        if (status == 404) { fail(gen, "expired"); return; }
        if (status != 200) continue;
        root = cJSON_Parse(w->resp);
        char state[16] = "";
        if (root) json_string(root, "status", state, sizeof(state));
        if (!strcmp(state, "pending")) {
            int left = json_int(root, "expires_in", -1);
            cJSON_Delete(root);
            if (left >= 0) {
                lock();
                if (s_generation == gen) s_status.expires_in = left;
                unlock();
            }
            continue;
        }
        if (!strcmp(state, "denied")) { cJSON_Delete(root); fail(gen, "denied"); return; }
        bool got = !strcmp(state, "approved") && json_string(root, "token", w->token, sizeof(w->token));
        cJSON_Delete(root);
        secure_zero(w->resp, sizeof(w->resp));
        if (!got || !token_acceptable(w->token)) { fail(gen, "invalid_response"); return; }
        if (!current(gen)) return;
        /* The operator may have changed or cleared the server, or moved to
         * Cloud, while the code was pending: a device token only belongs to
         * the server that issued it, so it is never stored for another one. */
        char now_url[LAN_URL_MAX + 1];
        bool now_token = false;
        stored_provisioning(now_url, sizeof(now_url), &now_token);
        cloud_auth_status_t now_cloud;
        cloud_auth_get_status(&now_cloud);
        if (strcmp(now_url, w->url) != 0 || now_cloud.pairing || now_cloud.paired) {
            secure_zero(w->token, sizeof(w->token));
            fail(gen, "server_changed");
            return;
        }
        esp_err_t err = net_ws_set_token(w->token);
        secure_zero(w->token, sizeof(w->token));
        if (err != ESP_OK) { fail(gen, "store_failed"); return; }
        /* A Box paired by its operator is ready for touch-to-talk. */
        (void)net_ws_set_agent_enabled(true);
        ESP_LOGI(TAG, "paired with %s; device token stored", w->url);
        set_phase(gen, LAN_PAIR_PAIRED, NULL);
        return;
    }
    fail(gen, "expired");
}

static void run_once(uint32_t gen, bool boot, bool discover_server, bool pair, lan_work_t *w)
{
    char stored[LAN_URL_MAX + 1];
    bool token_set = false;
    stored_provisioning(stored, sizeof(stored), &token_set);
    if (boot) {
        cloud_auth_status_t cloud;
        cloud_auth_get_status(&cloud);
        if (cloud.pairing || cloud.paired || kbx_cloud_server(stored)) {
            set_phase(gen, LAN_PAIR_IDLE, NULL);
            return;
        }
        if (stored[0] && token_set) {
            set_phase(gen, LAN_PAIR_IDLE, NULL);
            return;
        }
        discover_server = stored[0] == '\0';
        pair = true;
        ESP_LOGI(TAG, "boot: %s", discover_server ? "no server stored - looking for one on the LAN"
                                                : "LAN server without a device token - pairing");
    }
    set_phase(gen, discover_server ? LAN_PAIR_BROWSING : LAN_PAIR_REQUESTING, NULL);
    for (int waited = 0; !net_wifi_is_connected(NULL, 0); waited++) {
        if (waited >= LAN_WIFI_WAIT_S || !wait_current(gen, 1000)) {
            if (current(gen)) fail(gen, "offline");
            return;
        }
    }
    if (discover_server) {
        if (!discover(gen, w)) {
            if (current(gen)) fail(gen, "no_server");
            return;
        }
        set_server(gen, w->url, w->instance);
        if (strcmp(stored, w->url) != 0) {
            /* Another server: its token would not work there. */
            if (stored[0]) (void)net_ws_clear();
            if (net_ws_set_server(w->url) != ESP_OK) { fail(gen, "store_failed"); return; }
            token_set = false;
            ESP_LOGI(TAG, "server set from DNS-SD: %s (%s)", w->url, w->instance);
        }
        if (!pair && token_set) {
            ESP_LOGI(TAG, "server unchanged; device token kept");
            set_phase(gen, LAN_PAIR_IDLE, NULL);
            return;
        }
    } else {
        if (!stored[0] || kbx_cloud_server(stored)) { fail(gen, "no_server"); return; }
        strlcpy(w->url, stored, sizeof(w->url));
        set_server(gen, w->url, "");
    }
    pair_with(gen, w);
}

static void worker(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    lock();
    bool boot = s_q_boot, disc = s_q_discover, pair = s_q_pair;
    s_queued = false;
    unlock();
    lan_work_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        if (w) {
            run_once(gen, boot, disc, pair, w);
            secure_zero(w, sizeof(*w));
        } else {
            fail(gen, "out_of_memory");
        }
        lock();
        if (s_queued) {
            s_queued = false;
            gen = s_generation;
            boot = s_q_boot; disc = s_q_discover; pair = s_q_pair;
            unlock();
            continue;
        }
        s_running = false;
        unlock();
        break;
    }
    heap_caps_free(w);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t start(bool boot, bool discover_server, bool pair)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    lock();
    uint32_t gen = ++s_generation;
    s_q_boot = boot; s_q_discover = discover_server; s_q_pair = pair;
    if (s_running) {
        s_queued = true;   /* the running worker picks it up after cancelling */
        unlock();
        return ESP_OK;
    }
    s_running = true;
    unlock();
    if (xTaskCreatePinnedToCoreWithCaps(worker, "lan_pair", 8192, (void *)(uintptr_t)gen, 3, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        lock();
        s_running = false;
        unlock();
        fail(gen, "task_failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t lan_pair_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    return start(true, false, false);
}

esp_err_t lan_pair_start(bool discover_server, bool pair)
{
    return start(false, discover_server, pair);
}

void lan_pair_cancel(void)
{
    if (!s_lock) return;
    lock();
    ++s_generation;
    s_queued = false;
    s_status.phase = LAN_PAIR_IDLE;
    s_status.code[0] = '\0';
    s_status.error[0] = '\0';
    s_status.seq++;
    unlock();
    ESP_LOGI(TAG, "cancelled");
}

void lan_pair_get_status(lan_pair_status_t *out)
{
    if (!out) return;
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_status;
    unlock();
}

void lan_pair_log_status(void)
{
    static const char *const names[] = { "idle", "browsing", "requesting", "show_code", "paired", "failed" };
    lan_pair_status_t st;
    lan_pair_get_status(&st);
    ESP_LOGI(TAG, "status: phase=%s server=%s instance=%s code=%s expires=%d error=%s",
             st.phase < sizeof(names) / sizeof(names[0]) ? names[st.phase] : "?",
             st.server[0] ? st.server : "-", st.instance[0] ? st.instance : "-",
             st.code[0] ? st.code : "-", st.expires_in, st.error[0] ? st.error : "-");
}
