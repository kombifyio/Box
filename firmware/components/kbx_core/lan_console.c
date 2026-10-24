/*
 * kombify box - LAN console: remote KBX commands and firmware push over
 * Wi-Fi from a development PC.
 *
 * Off until a 32-byte key is set over USB ("KBX remote key <64 hex>"); the
 * key never leaves the device and is never logged. Every request carries a
 * single-use nonce from GET /kbx/nonce and an HMAC-SHA256 over
 *   "kbx-lan-v1\n" <nonce> "\n" <kind> "\n" <payload>
 * so a captured request cannot be replayed or altered.
 *   POST /kbx/cmd?wait=<ms>  kind "cmd", payload = the command line; the
 *                            reply is the device log for <wait> ms (<=10 s)
 *   POST /kbx/ota            kind "ota", payload = "<size>\n<sha256 hex>";
 *                            body = app image, checked before the slot
 *                            switch, then the box reboots
 *   POST /kbx/screen         kind "screen", empty payload; the panel frame
 * Commands that carry or change secrets (Wi-Fi password, tokens, this key)
 * and USB-only recovery are refused over the LAN: the transport is plain
 * HTTP, only authenticated, not encrypted.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "display.h"
#include "fw_ota.h"
#include "fw_update_manifest.h"
#include "lan_console.h"
#include "secure_zero.h"

static const char *TAG = "kbx_lan";

#define LAN_PORT          8266
#define LAN_NVS_NAMESPACE "kbx_lan"
#define LAN_NVS_KEY       "key"
#define LAN_KEY_BYTES     32
#define LAN_NONCE_BYTES   16
#define LAN_NONCE_TTL_US  (60LL * 1000 * 1000)
#define LAN_CMD_MAX       1024
#define LAN_CAPTURE_BYTES (16 * 1024)
#define LAN_WAIT_DEFAULT  1500
#define LAN_WAIT_MAX      10000
#define LAN_OTA_CHUNK     4096

static const lan_console_commands_t *s_commands;
static httpd_handle_t s_server;
static SemaphoreHandle_t s_lock;
static uint8_t s_key[LAN_KEY_BYTES];
static bool s_have_key;
static char s_nonce[LAN_NONCE_BYTES * 2 + 1];
static int64_t s_nonce_us;
static uint32_t s_ok, s_denied, s_refused;

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
}

static bool from_hex(const char *hex, uint8_t *out, size_t n)
{
    if (!hex || strlen(hex) != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = (char)tolower((unsigned char)hex[2 * i + k]);
            if (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned)(c - 'a' + 10);
            else return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

/* Consumes the nonce whatever the outcome; true only for a fresh nonce and
 * a matching MAC. */
static bool lan_authorize(httpd_req_t *req, const char *kind, const char *payload, size_t payload_len)
{
    char nonce[LAN_NONCE_BYTES * 2 + 1] = {0}, auth_hex[65] = {0};
    uint8_t auth[32];
    if (httpd_req_get_hdr_value_str(req, "X-KBX-Nonce", nonce, sizeof(nonce)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-KBX-Auth", auth_hex, sizeof(auth_hex)) != ESP_OK ||
        !from_hex(auth_hex, auth, sizeof(auth))) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool fresh = s_have_key && s_nonce[0] && strcmp(nonce, s_nonce) == 0 &&
                 esp_timer_get_time() - s_nonce_us < LAN_NONCE_TTL_US;
    s_nonce[0] = '\0';
    uint8_t key[LAN_KEY_BYTES];
    memcpy(key, s_key, sizeof(key));
    xSemaphoreGive(s_lock);
    if (!fresh) {
        secure_zero(key, sizeof(key));
        return false;
    }

    bool ok = false;
    psa_key_id_t id = 0;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    const size_t head_len = strlen("kbx-lan-v1\n") + strlen(nonce) + 1 + strlen(kind) + 1;
    char *msg = heap_caps_malloc(head_len + payload_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (msg && psa_crypto_init() == PSA_SUCCESS && psa_import_key(&attr, key, sizeof(key), &id) == PSA_SUCCESS) {
        int n = snprintf(msg, head_len + 1, "kbx-lan-v1\n%s\n%s\n", nonce, kind);
        memcpy(msg + n, payload, payload_len);
        ok = psa_mac_verify(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), (const uint8_t *)msg, (size_t)n + payload_len,
                            auth, sizeof(auth)) == PSA_SUCCESS;
    }
    if (id) psa_destroy_key(id);
    free(msg);
    secure_zero(key, sizeof(key));
    return ok;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t nonce_get(httpd_req_t *req)
{
    uint8_t raw[LAN_NONCE_BYTES];
    esp_fill_random(raw, sizeof(raw));
    char body[160];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    to_hex(raw, sizeof(raw), s_nonce);
    s_nonce_us = esp_timer_get_time();
    snprintf(body, sizeof(body), "{\"nonce\":\"%s\",\"board\":\"%s\",\"version\":\"%s\"}", s_nonce,
             FW_UPDATE_BOARD, esp_app_get_description()->version);
    xSemaphoreGive(s_lock);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* Secrets and USB-only recovery stay on the cable. */
static bool lan_command_refused(const char *line)
{
    while (*line && isspace((unsigned char)*line)) line++;
    if (strncasecmp(line, "KBX", 3) == 0 && isspace((unsigned char)line[3])) {
        line += 4;
        while (*line && isspace((unsigned char)*line)) line++;
    }
    /* The status queries "remote?" and "token?" stay allowed. */
    static const char *const prefixes[] = { "remote", "token", "bootloader", "ota-begin" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t n = strlen(prefixes[i]);
        if (strncasecmp(line, prefixes[i], n) == 0 && line[n] != '?' &&
            (line[n] == '\0' || isspace((unsigned char)line[n]) || line[n] == '-')) {
            return true;
        }
    }
    /* "wifi <ssid>|<pass>" carries the Wi-Fi password. */
    return strncasecmp(line, "wifi", 4) == 0 && strchr(line, '|') != NULL;
}

static esp_err_t cmd_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > LAN_CMD_MAX) return reply(req, "413 Payload Too Large", "command too long\n");
    char *line = heap_caps_calloc(1, LAN_CMD_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *log = heap_caps_malloc(LAN_CAPTURE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line || !log) {
        free(line); free(log);
        return reply(req, "503 Service Unavailable", "no memory\n");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, line + got, req->content_len - got);
        if (n <= 0) { free(line); free(log); return ESP_FAIL; }
        got += (size_t)n;
    }
    while (got && (line[got - 1] == '\n' || line[got - 1] == '\r')) line[--got] = '\0';

    if (!lan_authorize(req, "cmd", line, got)) {
        s_denied++;
        ESP_LOGW(TAG, "denied request from LAN");
        free(line); free(log);
        return reply(req, "401 Unauthorized", "denied\n");
    }
    if (lan_command_refused(line)) {
        s_refused++;
        free(line); free(log);
        return reply(req, "403 Forbidden", "this command is USB-only\n");
    }
    int wait = LAN_WAIT_DEFAULT;
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "wait", val, sizeof(val)) == ESP_OK) {
        wait = atoi(val);
        if (wait < 0) wait = 0;
        if (wait > LAN_WAIT_MAX) wait = LAN_WAIT_MAX;
    }
    s_ok++;
    bool captured = s_commands->capture_begin(log, LAN_CAPTURE_BYTES);
    s_commands->exec_line(line);
    vTaskDelay(pdMS_TO_TICKS(wait));
    if (captured) s_commands->capture_end();
    else snprintf(log, LAN_CAPTURE_BYTES, "(log capture busy)\n");
    esp_err_t err = reply(req, "200 OK", log);
    free(line);
    free(log);
    return err;
}

/* Panel frame for remote UI review: kind "screen", empty payload. */
static esp_err_t screen_post(httpd_req_t *req)
{
    if (!lan_authorize(req, "screen", "", 0)) {
        s_denied++;
        return reply(req, "401 Unauthorized", "denied\n");
    }
    const size_t cap = 32 * 1024;
    uint8_t *frame = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = frame ? display_frame_copy(frame, cap) : 0;
    if (!n) {
        free(frame);
        return reply(req, "404 Not Found", "no frame on this board\n");
    }
    s_ok++;
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t err = httpd_resp_send(req, (const char *)frame, (ssize_t)n);
    free(frame);
    return err;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    fw_ota_stream_reboot();
}

static esp_err_t ota_post(httpd_req_t *req)
{
    char sha_hex[65] = {0};
    uint8_t sha[32];
    if (httpd_req_get_hdr_value_str(req, "X-KBX-Sha256", sha_hex, sizeof(sha_hex)) != ESP_OK ||
        !from_hex(sha_hex, sha, sizeof(sha)) || req->content_len < 1024) {
        return reply(req, "400 Bad Request", "need X-KBX-Sha256 and an image body\n");
    }
    char payload[96];
    int plen = snprintf(payload, sizeof(payload), "%u\n%s", (unsigned)req->content_len, sha_hex);
    if (!lan_authorize(req, "ota", payload, (size_t)plen)) {
        s_denied++;
        ESP_LOGW(TAG, "denied update from LAN");
        return reply(req, "401 Unauthorized", "denied\n");
    }
    esp_err_t err = fw_ota_stream_begin(req->content_len, sha);
    if (err != ESP_OK) return reply(req, "409 Conflict", "update could not start\n");
    uint8_t *chunk = heap_caps_malloc(LAN_OTA_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        fw_ota_stream_abort("no-mem");
        return reply(req, "503 Service Unavailable", "no memory\n");
    }
    size_t got = 0;
    while (got < req->content_len) {
        size_t want = req->content_len - got;
        if (want > LAN_OTA_CHUNK) want = LAN_OTA_CHUNK;
        int n = httpd_req_recv(req, (char *)chunk, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || fw_ota_stream_write(chunk, (size_t)n) != ESP_OK) {
            free(chunk);
            fw_ota_stream_abort(n <= 0 ? "recv" : "write");
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    free(chunk);
    if (fw_ota_stream_finish() != ESP_OK) return reply(req, "422 Unprocessable Content", "image rejected\n");
    s_ok++;
    reply(req, "200 OK", "written, rebooting\n");
    xTaskCreate(reboot_task, "kbx_lan_rb", 3072, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t lan_start(void)
{
    if (s_server) return ESP_OK;
    if (!s_commands) return ESP_ERR_INVALID_STATE;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = LAN_PORT;
    cfg.ctrl_port = LAN_PORT + 1;
    /* Internal stack: the update handler writes flash, which unmaps PSRAM
     * stacks (fw_ota.c spawn_ota_task). */
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 3;
    cfg.max_uri_handlers = 5;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/kbx/nonce", .method = HTTP_GET, .handler = nonce_get },
        { .uri = "/kbx/cmd", .method = HTTP_POST, .handler = cmd_post },
        { .uri = "/kbx/ota", .method = HTTP_POST, .handler = ota_post },
        { .uri = "/kbx/screen", .method = HTTP_POST, .handler = screen_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    ESP_LOGI(TAG, "LAN console on port %d (signed requests only)", LAN_PORT);
    return ESP_OK;
}

static void lan_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "LAN console off");
}

static esp_err_t key_store(const uint8_t *key, size_t len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(LAN_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = key ? nvs_set_blob(nvs, LAN_NVS_KEY, key, len) : nvs_erase_key(nvs, LAN_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t lan_console_init(const lan_console_commands_t *commands)
{
    /* Without a complete table the console stays off (lan_start refuses). */
    s_commands = commands && commands->capture_begin && commands->exec_line && commands->capture_end
                     ? commands : NULL;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs;
    size_t len = sizeof(s_key);
    if (nvs_open(LAN_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        s_have_key = nvs_get_blob(nvs, LAN_NVS_KEY, s_key, &len) == ESP_OK && len == sizeof(s_key);
        nvs_close(nvs);
    }
    if (!s_have_key) {
        secure_zero(s_key, sizeof(s_key));
        ESP_LOGI(TAG, "LAN console off (no key; KBX remote key <64 hex> over USB)");
        return ESP_OK;
    }
    return lan_start();
}

bool lan_console_cdc_command(const char *args)
{
    while (*args && isspace((unsigned char)*args)) args++;
    if (*args == '?' || *args == '\0') {
        ESP_LOGI(TAG, "status: %s port=%d key=%s ok=%u denied=%u refused=%u", s_server ? "on" : "off", LAN_PORT,
                 s_have_key ? "set" : "none", (unsigned)s_ok, (unsigned)s_denied, (unsigned)s_refused);
        return true;
    }
    if (strncasecmp(args, "off", 3) == 0) {
        lan_stop();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_have_key = false;
        secure_zero(s_key, sizeof(s_key));
        s_nonce[0] = '\0';
        xSemaphoreGive(s_lock);
        esp_err_t err = key_store(NULL, 0);
        ESP_LOGI(TAG, "key cleared: %s", esp_err_to_name(err));
        return true;
    }
    if (strncasecmp(args, "key", 3) == 0 && isspace((unsigned char)args[3])) {
        args += 4;
        while (*args && isspace((unsigned char)*args)) args++;
        uint8_t key[LAN_KEY_BYTES];
        if (!from_hex(args, key, sizeof(key))) {
            ESP_LOGW(TAG, "key must be 64 hex characters");
            return true;
        }
        esp_err_t err = key_store(key, sizeof(key));
        if (err == ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memcpy(s_key, key, sizeof(s_key));
            s_have_key = true;
            s_nonce[0] = '\0';
            xSemaphoreGive(s_lock);
            err = lan_start();
        }
        secure_zero(key, sizeof(key));
        ESP_LOGI(TAG, "key set: %s", esp_err_to_name(err));
        return true;
    }
    ESP_LOGW(TAG, "use KBX remote key <64 hex> | remote off | remote?");
    return true;
}
