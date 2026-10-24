#include "c6_voice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"

#include "c6_adpcm.h"
#include "c6_audio.h"
#include "c6_wake.h"
#include "cloud_auth.h"
#include "net_wifi.h"
#include "session_url.h"
#include "voice_error.h"
#include "voice_protocol.h"

static const char *TAG = "c6_voice";

#define FRAME_SAMPLES     320                      /* 20 ms at 16 kHz */
#define RING_BYTES        (32 * 1024)              /* ~0.7 s of 24 kHz reply */
#define PLAY_CHUNK        1920                     /* 40 ms at 24 kHz */
#define CONNECT_MAX_US    ((int64_t)20 * 1000000)
#define LISTEN_MAX_US     ((int64_t)20 * 1000000)
#define NO_SPEECH_US      ((int64_t)10 * 1000000)
#define THINK_MAX_US      ((int64_t)30 * 1000000)
#define REPLY_IDLE_US     ((int64_t)8 * 1000000)
#define REPLY_MAX_US      ((int64_t)90 * 1000000)
#define ERROR_SHOW_MS     4000

static volatile c6_voice_state_t s_state;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static volatile bool s_tap;
static esp_websocket_client_handle_t s_ws;
static RingbufHandle_t s_ring;
static kbx_voice_text_t s_text;
static StreamBufferHandle_t s_inject;

/* Capture FIFO: from the tap or wake word until the uplink has caught up,
 * the microphone runs into 4:1 ADPCM (32 KB, ~4 s at 16 kHz), so speech
 * during account renewal, session mint and the WSS dial is kept. Static,
 * so it never fragments the heap the TLS handshakes need. */
/* 52 KB: ~6.5 s of ADPCM, and large enough to lend to cloud_auth as its
 * renewal work area during idle upkeep (cloud_auth_work_size, ~51 KB). */
#define FIFO_BYTES (52 * 1024)
/* Each FIFO frame is self-contained: the encoder state (predictor, step
 * index) in a 4-byte header, then 20 ms of codes. Old pre-roll frames can be
 * dropped without desynchronising the decoder. */
#define ADPCM_CODES (FRAME_SAMPLES / 2)
#define ADPCM_FRAME (4 + ADPCM_CODES)
#define PREROLL_FRAMES 75   /* 1.5 s kept before a wake word hit */
static uint8_t s_fifo_storage[FIFO_BYTES + 1];
static StaticStreamBuffer_t s_fifo_struct;
static StreamBufferHandle_t s_fifo;
static TaskHandle_t s_cap_task;
static volatile bool s_capture;
static volatile bool s_capture_idle = true;
static volatile uint32_t s_capture_drops;
static c6_adpcm_t s_enc, s_dec;
static uint8_t s_adpcm[ADPCM_FRAME];
static volatile bool s_wake_turn;
static char s_wake_phrase[24];
static float s_wake_score;
static volatile bool s_verifying, s_wake_rejected;
static bool s_server_verifies;

/* Warm-up: an idle dial that only collects the WSS TLS ticket. */
static volatile bool s_warm;
static int64_t s_ws_ticket_us;
#define TICKET_MAX_AGE_US ((int64_t)3600 * 1000 * 1000)
#define AUTH_MIN_VALID_S 150

/* Written by the websocket task, read by the voice task. */
static volatile bool s_ws_open, s_ws_closed, s_listening, s_input_heard, s_input_final;
static volatile bool s_turn_end, s_output_seen, s_downlink, s_accept_downlink, s_failed;
static volatile int64_t s_last_downlink_us;
static volatile uint8_t s_last_opcode;
static volatile uint32_t s_downlink_bytes;
static volatile int s_ws_http_status;
static volatile int64_t s_ws_dial_us;
/* Persistent WSS transport (TLS under WebSocket) so its session ticket
 * survives between turns; see api_client for why resumption matters. */
static esp_transport_handle_t s_ssl, s_wss;
static bool s_ws_ticket;

static kbx_voice_error_t s_err;
static bool s_has_err;
static char s_caption[256];
static bool s_caption_agent;
static uint32_t s_caption_seq;
static char s_locale[8] = "de-DE";

static char *s_auth;          /* "Bearer <access token>", wiped after the turn */
static char s_sid[64];
static char s_subproto[256];
static char s_ws_url[192];
static char s_http_url[192];
static char s_resp[4096];
static int16_t s_frame[FRAME_SAMPLES];
static int16_t s_play[PLAY_CHUNK / 2 + 1];

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void set_state(c6_voice_state_t state)
{
    if (s_state != state) ESP_LOGI(TAG, "state %d -> %d", (int)s_state, (int)state);
    s_state = state;
}

static void fail(kbx_voice_error_kind_t kind, int status, const char *code)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_has_err) {
        kbx_voice_error_set(&s_err, kind, status, code);
        s_has_err = true;
        ESP_LOGW(TAG, "turn failed: %s %d %s", kbx_voice_error_name(s_err.kind), status, s_err.code);
    }
    s_failed = true;
    xSemaphoreGive(s_lock);
}

static bool take_tap(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool tap = s_tap;
    s_tap = false;
    xSemaphoreGive(s_lock);
    return tap;
}

static void set_caption(const char *text, bool agent)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_caption, text, sizeof(s_caption));
    s_caption_agent = agent;
    s_caption_seq++;
    xSemaphoreGive(s_lock);
}

static void drain_ring(void)
{
    size_t n = 0;
    void *p;
    if (!s_ring) return;
    while ((p = xRingbufferReceiveUpTo(s_ring, &n, 0, RING_BYTES)) != NULL) vRingbufferReturnItem(s_ring, p);
}

static size_t ring_waiting(void)
{
    UBaseType_t waiting = 0;
    if (!s_ring) return 0;
    vRingbufferGetInfo(s_ring, NULL, NULL, NULL, NULL, &waiting);
    return (size_t)waiting;
}

/* ── HTTPS to the Gateway ─────────────────────────────────────────────── */

/* One TLS transport to the Gateway host carries session create, the WSS
 * media socket and DELETE, one connection at a time. Let's Encrypt's P-384
 * chain costs the C6 about a second of software ECDSA per full handshake
 * (only P-256 has hardware), so the TLS 1.3 session ticket is kept and
 * every later connection resumes. TLS 1.3 sends the ticket after the
 * handshake, so it is saved only after a response was read. */
static esp_err_t tls_transport(void)
{
    if (s_ssl) return ESP_OK;
    s_ssl = esp_transport_ssl_init();
    if (!s_ssl) return ESP_ERR_NO_MEM;
    esp_transport_ssl_crt_bundle_attach(s_ssl, esp_crt_bundle_attach);
    esp_transport_set_default_port(s_ssl, 443);
    esp_transport_ssl_session_ticket_operation(s_ssl, ESP_TRANSPORT_SESSION_TICKET_INIT);
    return ESP_OK;
}

static void tls_ticket_save(void)
{
    if (esp_transport_ssl_session_ticket_operation(s_ssl, ESP_TRANSPORT_SESSION_TICKET_SAVE) == ESP_OK) {
        s_ws_ticket = true;
        s_ws_ticket_us = esp_timer_get_time();
    }
}

static bool tls_write(const char *p, size_t n)
{
    while (n > 0) {
        int w = esp_transport_write(s_ssl, p, (int)n, 10000);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Case-insensitive header lookup inside the header block. */
static const char *header_value(const char *headers, const char *name)
{
    size_t n = strlen(name);
    for (const char *line = headers; line && *line; line = strstr(line, "\r\n")) {
        if (line[0] == '\r') line += 2;
        if (!strncasecmp(line, name, n) && line[n] == ':') {
            line += n + 1;
            while (*line == ' ') line++;
            return line;
        }
    }
    return NULL;
}

/* De-chunks in place; returns the body length or -1. */
static int dechunk(char *body, size_t len)
{
    size_t in = 0, out = 0;
    while (in < len) {
        char *end = NULL;
        unsigned long size = strtoul(body + in, &end, 16);
        char *crlf = strstr(body + in, "\r\n");
        if (!crlf || end > crlf) return -1;
        in = (size_t)(crlf - body) + 2;
        if (size == 0) return (int)out;
        if (in + size > len) return -1;
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
    return -1;
}

static esp_err_t http_call(const char *method, const char *url, const char *body, int *status)
{
    *status = 0;
    s_resp[0] = 0;
    if (tls_transport() != ESP_OK) return ESP_ERR_NO_MEM;
    const char *host_start = url + strlen("https://");
    const char *path = strchr(host_start, '/');
    char host[64];
    size_t host_len = path ? (size_t)(path - host_start) : 0;
    if (strncmp(url, "https://", 8) || !path || host_len == 0 || host_len >= sizeof(host)) return ESP_ERR_INVALID_ARG;
    memcpy(host, host_start, host_len);
    host[host_len] = 0;

    bool resumed = s_ws_ticket;
    if (resumed) esp_transport_ssl_session_ticket_operation(s_ssl, ESP_TRANSPORT_SESSION_TICKET_USE);
    int64_t t0 = esp_timer_get_time();
    if (esp_transport_connect(s_ssl, host, 443, 10000) < 0) {
        ESP_LOGW(TAG, "https connect failed (heap %u, largest %u)", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_transport_close(s_ssl);
        return ESP_ERR_HTTP_CONNECT;
    }
    ESP_LOGI(TAG, "https connect %d ms (%s)", (int)((esp_timer_get_time() - t0) / 1000),
             resumed ? "resumed" : "full handshake");
    size_t body_len = body ? strlen(body) : 0;
    char head[320];
    int n = snprintf(head, sizeof(head),
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: kombify-box-c6\r\nConnection: close\r\n"
                     "Content-Length: %u\r\n%sAuthorization: ",
                     method, path, host, (unsigned)body_len, body ? "Content-Type: application/json\r\n" : "");
    bool ok = n > 0 && (size_t)n < sizeof(head) && tls_write(head, (size_t)n) && tls_write(s_auth, strlen(s_auth)) &&
              tls_write("\r\n\r\n", 4) && (!body_len || tls_write(body, body_len));
    size_t used = 0;
    char *split = NULL;
    long want = -1;
    while (ok) {
        if (used >= sizeof(s_resp) - 1) {
            ok = false;
            break;
        }
        int r = esp_transport_read(s_ssl, s_resp + used, (int)(sizeof(s_resp) - 1 - used), 10000);
        if (r <= 0) break;
        used += (size_t)r;
        s_resp[used] = 0;
        if (!split && (split = strstr(s_resp, "\r\n\r\n")) != NULL) {
            *split = 0;
            const char *cl = header_value(s_resp, "Content-Length");
            if (cl) want = strtol(cl, NULL, 10);
            *split = '\r';
        }
        if (split && want >= 0 && used >= (size_t)(split + 4 - s_resp) + (size_t)want) break;
    }
    tls_ticket_save();
    esp_transport_close(s_ssl);
    if (!ok || !split) {
        ESP_LOGW(TAG, "https %s failed after %u B", method, (unsigned)used);
        return ok ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
    }
    *split = 0;
    if (sscanf(s_resp, "HTTP/1.%*c %d", status) != 1) return ESP_FAIL;
    const char *te = header_value(s_resp, "Transfer-Encoding");
    bool chunked = te && !strncasecmp(te, "chunked", 7);
    char *payload = split + 4;
    size_t payload_len = used - (size_t)(payload - s_resp);
    int body_out = chunked ? dechunk(payload, payload_len) : (int)payload_len;
    if (body_out < 0) return ESP_FAIL;
    memmove(s_resp, payload, (size_t)body_out);
    s_resp[body_out] = 0;
    return ESP_OK;
}

static esp_err_t load_auth(void)
{
    char *token = heap_caps_malloc(CLOUD_AUTH_TOKEN_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!token) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "no_memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = cloud_auth_access_token(token, CLOUD_AUTH_TOKEN_MAX);
    if (err != ESP_OK) {
        cloud_auth_status_t st;
        cloud_auth_get_status(&st);
        ESP_LOGW(TAG, "cloud credential unavailable (%s)", st.error_code[0] ? st.error_code : "-");
        if (!st.paired) fail(KBX_VOICE_ERR_SIGNIN, 0, "signed_out");
        else if (!strcmp(st.error_code, "refresh_transport_failed"))
            fail(net_wifi_is_connected(NULL, 0) ? KBX_VOICE_ERR_SERVICE : KBX_VOICE_ERR_OFFLINE, 0, "auth_connect");
        else if (!strcmp(st.error_code, "clock_not_ready")) fail(KBX_VOICE_ERR_SERVICE, 0, "clock_not_ready");
        else fail(KBX_VOICE_ERR_SIGNIN, 0, st.error_code[0] ? st.error_code : "signin");
        wipe(token, CLOUD_AUTH_TOKEN_MAX);
        free(token);
        return ESP_ERR_INVALID_STATE;
    }
    size_t n = strlen(token) + sizeof("Bearer ");
    s_auth = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_auth) snprintf(s_auth, n, "Bearer %s", token);
    wipe(token, CLOUD_AUTH_TOKEN_MAX);
    free(token);
    if (!s_auth) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "no_memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void free_auth(void)
{
    if (!s_auth) return;
    wipe(s_auth, strlen(s_auth));
    free(s_auth);
    s_auth = NULL;
}

static esp_err_t mint_session(void)
{
    if (!kbx_session_url(KBX_CLOUD_SERVER, NULL, false, s_http_url, sizeof(s_http_url))) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "server_url");
        return ESP_ERR_INVALID_ARG;
    }
    int status = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = http_call("POST", s_http_url, "{}", &status);
    if (err != ESP_OK) {
        fail(net_wifi_is_connected(NULL, 0) ? KBX_VOICE_ERR_SERVICE : KBX_VOICE_ERR_OFFLINE, 0,
             err == ESP_ERR_INVALID_SIZE ? "response_too_large" : "connect");
        return err;
    }
    ESP_LOGI(TAG, "session-create http %d in %d ms", status, (int)((esp_timer_get_time() - t0) / 1000));
    if (status < 200 || status >= 300) {
        kbx_voice_error_t e;
        kbx_voice_error_from_http(status, s_resp, &e);
        ESP_LOGW(TAG, "session-create refused: http %d code=%s reason=%s request_id=%s", status, e.code,
                 e.reason[0] ? e.reason : "-", e.request_id[0] ? e.request_id : "-");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_has_err) { s_err = e; s_has_err = true; }
        s_failed = true;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(s_resp);
    const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON *jsub = cJSON_GetObjectItemCaseSensitive(root, "ws_subprotocol");
    bool ok = cJSON_IsString(jsid) && cJSON_IsString(jsub) &&
        strlcpy(s_sid, jsid->valuestring, sizeof(s_sid)) < sizeof(s_sid) &&
        strlcpy(s_subproto, jsub->valuestring, sizeof(s_subproto)) < sizeof(s_subproto) &&
        /* Only dial the authority that minted the ticket, never a URL from the response. */
        kbx_session_url(KBX_CLOUD_SERVER, s_sid, true, s_ws_url, sizeof(s_ws_url));
    cJSON_Delete(root);
    wipe(s_resp, sizeof(s_resp));
    if (!ok) {
        s_sid[0] = '\0';
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "bad_session_response");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "session %s minted", s_sid);
    return ESP_OK;
}

static void delete_session(void)
{
    if (!s_sid[0] || !s_auth) return;
    if (kbx_session_url(KBX_CLOUD_SERVER, s_sid, false, s_http_url, sizeof(s_http_url))) {
        int status = 0;
        if (http_call("DELETE", s_http_url, NULL, &status) == ESP_OK)
            ESP_LOGI(TAG, "session %s deleted (http %d)", s_sid, status);
    }
    s_sid[0] = '\0';
}

/* ── WebSocket ────────────────────────────────────────────────────────── */

static void send_text(const char *text)
{
    if (s_ws && s_ws_open)
        esp_websocket_client_send_text(s_ws, text, (int)strlen(text), pdMS_TO_TICKS(1000));
}

static void handle_text(const char *buf, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(buf, length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "bad_frame");
        return;
    }
    unsigned phases = kbx_voice_phases(root);
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(jt) ? jt->valuestring : "";
    const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "state");
    ESP_LOGI(TAG, "rx %s%s%s len=%u", type, cJSON_IsString(js) ? " state=" : "",
             cJSON_IsString(js) ? js->valuestring : "", (unsigned)length);
    if (!strcmp(type, "state") && cJSON_IsString(js) && !strcmp(js->valuestring, "verifying")) {
        /* Two-stage wake word: the server wants the pre-roll and speech now. */
        s_verifying = true;
        s_listening = true;
    } else if (!strcmp(type, "wake_verified")) {
        ESP_LOGI(TAG, "wake word verified by the server");
    } else if (!strcmp(type, "wake_rejected")) {
        ESP_LOGI(TAG, "wake word rejected by the server");
        s_wake_rejected = true;
    } else if (!strcmp(type, "state") && cJSON_IsString(js) && !strcmp(js->valuestring, "listening")) {
        s_listening = true;
    } else if (!strcmp(type, "error")) {
        const cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "code");
        fail(KBX_VOICE_ERR_NONE, 0, cJSON_IsString(jc) ? jc->valuestring : "server_error");
    } else if (!strcmp(type, "input_transcript") || !strcmp(type, "output_transcript")) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring[0])
            set_caption(text->valuestring, type[0] == 'o');
    }
    if (phases & KBX_VOICE_INPUT_HEARD) s_input_heard = true;
    if (phases & KBX_VOICE_INPUT_FINAL) s_input_final = true;
    if (phases & KBX_VOICE_OUTPUT_SEEN) s_output_seen = true;
    if (kbx_voice_turn_ended(phases, s_output_seen)) s_turn_end = true;
    if (phases & KBX_VOICE_SESSION_END) s_ws_closed = true;
    cJSON_Delete(root);
}

static void ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        s_ws_open = true;
        ESP_LOGI(TAG, "ws dial %d ms (%s)", (int)((esp_timer_get_time() - s_ws_dial_us) / 1000),
                 s_ws_ticket ? "resumed" : "full handshake");
        tls_ticket_save();
        /* A warm-up dial only collects the TLS ticket: no start frame, so
         * no provider session and nothing billed. */
        if (s_warm) break;
        char start[256];
        char wake[96] = "";
        if (s_wake_turn)
            snprintf(wake, sizeof(wake), ",\"wake\":{\"phrase\":\"%s\",\"score\":%.2f,\"preroll_ms\":%d}",
                     s_wake_phrase, (double)s_wake_score, PREROLL_FRAMES * 20);
        snprintf(start, sizeof(start),
                 "{\"type\":\"start\",\"media_transport\":\"websocket\",\"locale\":\"%s\","
                 "\"activity_detection\":{\"automatic\":true}%s}", s_locale, wake);
        ESP_LOGI(TAG, "ws connected (heap %u) - start locale=%s", (unsigned)esp_get_free_heap_size(), s_locale);
        esp_websocket_client_send_text(s_ws, start, (int)strlen(start), pdMS_TO_TICKS(2000));
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (!ev) break;
        if (ev->op_code == 0x08) {
            int code = ev->data_len >= 2 ? (((uint8_t)ev->data_ptr[0] << 8) | (uint8_t)ev->data_ptr[1]) : 1005;
            ESP_LOGI(TAG, "rx close code=%d", code);
            s_ws_closed = true;
            break;
        }
        if (ev->op_code == 0x09 || ev->op_code == 0x0A) break;
        uint8_t op = (uint8_t)ev->op_code;
        if (op != 0x00) s_last_opcode = op;
        bool binary = op == 0x02 || (op == 0x00 && s_last_opcode == 0x02);
        if (binary) {
            if (!s_accept_downlink || !s_ring || !ev->data_ptr || ev->data_len <= 0) break;
            if (!s_downlink) {
                s_downlink = true;
                ESP_LOGI(TAG, "downlink start %d B", ev->data_len);
            }
            s_last_downlink_us = esp_timer_get_time();
            s_downlink_bytes += (uint32_t)ev->data_len;
            /* Blocking send is the backpressure: TCP stalls while the speaker drains. */
            if (xRingbufferSend(s_ring, ev->data_ptr, (size_t)ev->data_len, pdMS_TO_TICKS(2000)) != pdTRUE)
                ESP_LOGW(TAG, "downlink overrun, %d B dropped", ev->data_len);
            break;
        }
        if (ev->data_len >= 0 && ev->payload_len >= 0 && ev->payload_offset >= 0) {
            kbx_voice_text_result_t r = kbx_voice_text_feed(&s_text, op, ev->fin, (size_t)ev->payload_len,
                (size_t)ev->payload_offset, ev->data_ptr, (size_t)ev->data_len);
            if (r == KBX_VOICE_TEXT_COMPLETE) {
                handle_text(s_text.bytes, s_text.used);
                kbx_voice_text_reset(&s_text);
            } else if (r == KBX_VOICE_TEXT_INVALID) {
                ESP_LOGW(TAG, "invalid or oversized control message");
                fail(KBX_VOICE_ERR_UNKNOWN, 0, "bad_frame");
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (ev && ev->error_handle.esp_ws_handshake_status_code > 0)
            s_ws_http_status = ev->error_handle.esp_ws_handshake_status_code;
        ESP_LOGW(TAG, "ws error: type=%d http=%d tls=0x%x heap=%u", ev ? (int)ev->error_handle.error_type : -1,
                 ev ? ev->error_handle.esp_ws_handshake_status_code : 0,
                 ev ? ev->error_handle.esp_tls_last_esp_err : 0, (unsigned)esp_get_free_heap_size());
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_ws_open = false;
        s_ws_closed = true;
        break;
    default:
        break;
    }
}


static esp_err_t ws_transport(void)
{
    if (tls_transport() != ESP_OK) return ESP_ERR_NO_MEM;
    if (!s_wss) {
        s_wss = esp_transport_ws_init(s_ssl);
        if (!s_wss) return ESP_ERR_NO_MEM;
        esp_transport_set_default_port(s_wss, 443);
    }
    const char *path = strchr(s_ws_url + strlen("wss://"), '/');
    esp_transport_ws_config_t ws = {
        .ws_path = path ? path : "/",
        .sub_protocol = s_subproto,
        .propagate_control_frames = true,
    };
    if (esp_transport_ws_set_config(s_wss, &ws) != ESP_OK) return ESP_FAIL;
    if (s_ws_ticket) esp_transport_ssl_session_ticket_operation(s_ssl, ESP_TRANSPORT_SESSION_TICKET_USE);
    return ESP_OK;
}

static esp_err_t ws_dial(void)
{
    if (ws_transport() != ESP_OK) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "no_memory");
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_client_config_t cfg = {
        .uri = s_ws_url,
        .subprotocol = s_subproto,
        .ext_transport = s_wss,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 15000,
        .ping_interval_sec = 15,
        .task_stack = 10240,
        .task_prio = 6,
        .buffer_size = 2048,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "no_memory");
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_handler, NULL);
    s_ws_dial_us = esp_timer_get_time();
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) fail(KBX_VOICE_ERR_SERVICE, 0, "ws_start");
    return err;
}

static void ws_close(void)
{
    if (!s_ws) return;
    s_accept_downlink = false;
    drain_ring();
    if (s_ws_open) {
        send_text("{\"type\":\"stop\"}");
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
    }
    drain_ring();
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_ws_open = false;
}

/* ── One turn ─────────────────────────────────────────────────────────── */

static bool cancelled(void)
{
    if (take_tap()) {
        ESP_LOGI(TAG, "cancelled by tap");
        fail(KBX_VOICE_ERR_NONE, 0, "cancelled");
        return true;
    }
    return false;
}

static void play_reply(void)
{
    set_state(C6_VOICE_SPEAKING);
    if (c6_audio_spk_open(C6_REPLY_RATE) != ESP_OK) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "speaker");
        return;
    }
    int64_t start = esp_timer_get_time();
    uint8_t carry = 0;
    bool has_carry = false;
    for (;;) {
        if (cancelled()) break;
        size_t n = 0;
        uint8_t *p = xRingbufferReceiveUpTo(s_ring, &n, pdMS_TO_TICKS(40), PLAY_CHUNK);
        if (p) {
            uint8_t *dst = (uint8_t *)s_play;
            size_t used = 0;
            if (has_carry) dst[used++] = carry;
            memcpy(dst + used, p, n);
            used += n;
            vRingbufferReturnItem(s_ring, p);
            has_carry = used & 1U;
            if (has_carry) carry = dst[--used];
            if (used) c6_audio_spk_write(s_play, used / 2);
            continue;
        }
        int64_t now = esp_timer_get_time();
        if ((s_turn_end || s_ws_closed || s_failed) && ring_waiting() == 0) break;
        if (now - s_last_downlink_us > REPLY_IDLE_US || now - start > REPLY_MAX_US) {
            ESP_LOGW(TAG, "reply ended without turn_end");
            break;
        }
    }
    /* Let the DMA run out on silence instead of cutting the last word. */
    memset(s_play, 0, sizeof(s_play));
    c6_audio_spk_write(s_play, PLAY_CHUNK / 2);
    c6_audio_spk_close();
    ESP_LOGI(TAG, "reply played: %u B", (unsigned)s_downlink_bytes);
}

static void fifo_encode(const int16_t *pcm, uint8_t *out)
{
    out[0] = (uint8_t)(s_enc.predictor & 0xFF);
    out[1] = (uint8_t)((s_enc.predictor >> 8) & 0xFF);
    out[2] = (uint8_t)s_enc.index;
    out[3] = 0;
    c6_adpcm_encode(&s_enc, pcm, FRAME_SAMPLES, out + 4);
}

static void fifo_decode(const uint8_t *in, int16_t *pcm)
{
    s_dec.predictor = (int16_t)(in[0] | (in[1] << 8));
    s_dec.index = in[2] > 88 ? 88 : in[2];
    c6_adpcm_decode(&s_dec, in + 4, ADPCM_CODES, pcm);
}

/* Wake engine (idle): keeps the last 1.5 s so the server hears the phrase. */
void c6_voice_preroll_push(const int16_t *pcm)
{
    static uint8_t frame[ADPCM_FRAME], drop[ADPCM_FRAME];
    if (s_capture || !s_fifo) return;
    while (xStreamBufferBytesAvailable(s_fifo) >= PREROLL_FRAMES * ADPCM_FRAME)
        xStreamBufferReceive(s_fifo, drop, sizeof(drop), 0);
    fifo_encode(pcm, frame);
    xStreamBufferSend(s_fifo, frame, sizeof(frame), 0);
}

/* ── Capture into the FIFO ─────────────────────────────────────────────── */

static void capture_task(void *arg)
{
    (void)arg;
    static int16_t pcm[FRAME_SAMPLES];
    static uint8_t enc[ADPCM_FRAME];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_capture_idle = false;
        uint32_t n = 0, errs = 0;
        while (s_capture) {
            int64_t t_read = esp_timer_get_time();
            esp_err_t rerr = c6_audio_mic_read(pcm, FRAME_SAMPLES);
            int32_t read_ms = (int32_t)((esp_timer_get_time() - t_read) / 1000);
            if (read_ms > 100) ESP_LOGW(TAG, "capture: mic read took %d ms", (int)read_ms);
            if (rerr != ESP_OK) {
                if (errs++ == 0) ESP_LOGW(TAG, "capture: mic read %s", esp_err_to_name(rerr));
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (++n % 100 == 0) ESP_LOGI(TAG, "capture: %u frames, fifo %u B", (unsigned)n,
                                         (unsigned)xStreamBufferBytesAvailable(s_fifo));
            if (s_inject) {
                /* Operator test input (KBX inject) replaces the microphone
                 * samples; silence follows once the clip ends. */
                size_t got = 0;
                /* Whole frames: a short read padded with zeros would chop the clip. */
                for (int i = 0; i < 10 && got < sizeof(pcm); ++i)
                    got += xStreamBufferReceive(s_inject, (uint8_t *)pcm + got, sizeof(pcm) - got, pdMS_TO_TICKS(20));
                if (got < sizeof(pcm)) memset((uint8_t *)pcm + got, 0, sizeof(pcm) - got);
            }
            fifo_encode(pcm, enc);
            /* Whole frames only, so encoder and decoder stay in step. */
            if (xStreamBufferSpacesAvailable(s_fifo) >= sizeof(enc)) xStreamBufferSend(s_fifo, enc, sizeof(enc), 0);
            else s_capture_drops++;
        }
        s_capture_idle = true;
    }
}

static void capture_start(void)
{
    /* A wake turn keeps the pre-roll the engine left in the FIFO. */
    if (!s_wake_turn) {
        xStreamBufferReset(s_fifo);
        c6_adpcm_reset(&s_enc);
    }
    c6_adpcm_reset(&s_dec);
    s_capture_drops = 0;
    s_capture = true;
    xTaskNotifyGive(s_cap_task);
}

static void capture_stop(void)
{
    s_capture = false;
    for (int i = 0; i < 50 && !s_capture_idle; ++i) vTaskDelay(pdMS_TO_TICKS(5));
}

/* ── One turn ─────────────────────────────────────────────────────────── */

static void reset_turn_state(void)
{
    s_ws_open = s_ws_closed = s_listening = s_input_heard = s_input_final = false;
    s_turn_end = s_output_seen = s_downlink = s_failed = false;
    s_verifying = s_wake_rejected = false;
    s_accept_downlink = true;
    s_downlink_bytes = 0;
    s_ws_http_status = 0;
    s_last_opcode = 0;
    kbx_voice_text_reset(&s_text);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_has_err = false;
    memset(&s_err, 0, sizeof(s_err));
    xSemaphoreGive(s_lock);
}

static void run_turn(void)
{
    reset_turn_state();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_caption[0] = 0;
    s_caption_seq++;
    xSemaphoreGive(s_lock);

    set_state(C6_VOICE_CONNECTING);
    int64_t t0 = esp_timer_get_time();
    /* The wake engine hands over the microphone and frees its RAM. */
    c6_wake_pause();
    ESP_LOGI(TAG, "turn start (heap %u, largest %u)", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    if (!net_wifi_is_connected(NULL, 0)) {
        fail(KBX_VOICE_ERR_OFFLINE, 0, "wifi");
        goto done;
    }
    if (!st.paired) {
        fail(KBX_VOICE_ERR_SIGNIN, 0, "signed_out");
        goto done;
    }
    if (c6_audio_mic_open() != ESP_OK) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "microphone");
        goto done;
    }
    capture_start();
    if (load_auth() != ESP_OK || cancelled()) goto done;
    if (mint_session() != ESP_OK || cancelled()) goto done;
    if (ws_dial() != ESP_OK) goto done;

    while (!s_listening) {
        /* The reply buffer exists only once the socket is up, so the account
         * refresh and the TLS handshakes before it keep 32 KB more. */
        if (s_ws_open && !s_ring && !(s_ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_BYTEBUF))) {
            fail(KBX_VOICE_ERR_UNKNOWN, 0, "no_memory");
            goto done;
        }
        if (cancelled() || s_failed) goto done;
        if (s_ws_closed || esp_timer_get_time() - t0 > CONNECT_MAX_US) {
            fail(KBX_VOICE_ERR_SERVICE, s_ws_http_status, s_ws_http_status ? "ws_upgrade" : "connect_timeout");
            goto done;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    size_t backlog = xStreamBufferBytesAvailable(s_fifo);
    ESP_LOGI(TAG, "listening after %d ms, %u ms of speech buffered (%u frames dropped)",
             (int)((esp_timer_get_time() - t0) / 1000), (unsigned)(backlog / ADPCM_FRAME * 20U), (unsigned)s_capture_drops);

    set_state(C6_VOICE_LISTENING);
    int64_t listen_start = esp_timer_get_time();
    uint32_t frames = 0;
    bool silent = false;
    for (;;) {
        if (take_tap()) {
            ESP_LOGI(TAG, "tap ends the utterance");
            break;
        }
        if (s_input_final || s_downlink || s_failed || s_ws_closed) break;
        int64_t now = esp_timer_get_time();
        if (now - listen_start > LISTEN_MAX_US) break;
        if (!s_input_heard && now - listen_start > NO_SPEECH_US) {
            silent = true;
            break;
        }
        size_t got = xStreamBufferReceive(s_fifo, s_adpcm, sizeof(s_adpcm), pdMS_TO_TICKS(100));
        if (got == 0) continue;
        for (int i = 0; i < 5 && got < sizeof(s_adpcm); ++i)
            got += xStreamBufferReceive(s_fifo, s_adpcm + got, sizeof(s_adpcm) - got, pdMS_TO_TICKS(20));
        if (got < sizeof(s_adpcm)) continue;
        fifo_decode(s_adpcm, s_frame);
        if (esp_websocket_client_send_bin(s_ws, (const char *)s_frame, (int)sizeof(s_frame), pdMS_TO_TICKS(500)) < 0) {
            fail(KBX_VOICE_ERR_SERVICE, 0, "uplink");
            break;
        }
        frames++;
    }
    capture_stop();
    c6_audio_mic_close();
    ESP_LOGI(TAG, "uplink %u frames (heard=%d final=%d, %u dropped)", (unsigned)frames, s_input_heard,
             s_input_final, (unsigned)s_capture_drops);
    if (silent) {
        fail(KBX_VOICE_ERR_NONE, 0, "no_speech");
        goto done;
    }
    if (s_failed || s_ws_closed) goto done;
    if (!s_downlink) send_text("{\"type\":\"audio_end\"}");

    set_state(C6_VOICE_THINKING);
    int64_t think_start = esp_timer_get_time();
    while (!s_downlink) {
        if (cancelled() || s_failed) goto done;
        if (s_turn_end || s_ws_closed) break;
        if (esp_timer_get_time() - think_start > THINK_MAX_US) {
            fail(KBX_VOICE_ERR_SERVICE, 0, "reply_timeout");
            goto done;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_downlink) play_reply();

done:
    capture_stop();
    c6_audio_mic_close();
    ws_close();
    if (s_ring) {
        vRingbufferDelete(s_ring);
        s_ring = NULL;
    }
    delete_session();
    free_auth();
    take_tap();
    ESP_LOGI(TAG, "turn end after %d ms (heap %u)", (int)((esp_timer_get_time() - t0) / 1000),
             (unsigned)esp_get_free_heap_size());
    if (s_wake_turn && s_ws_open && s_verifying != s_server_verifies) {
        s_server_verifies = s_verifying;
        ESP_LOGI(TAG, "server %s wake words", s_server_verifies ? "verifies" : "does not verify");
    }
    /* A rejected candidate is the expected outcome of a low device cutoff:
     * back to idle without an error. */
    bool show_error = s_has_err && strcmp(s_err.code, "cancelled") != 0 && !s_wake_rejected;
    s_wake_turn = false;
    if (show_error) {
        set_state(C6_VOICE_ERROR);
        for (int i = 0; i < ERROR_SHOW_MS / 50; ++i) {
            if (take_tap()) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    set_state(C6_VOICE_IDLE);
    c6_wake_resume();
}

/* ── Idle upkeep ──────────────────────────────────────────────────────── */

/* Keeps the first spoken turn fast: renew the account token before it
 * runs out and hold a fresh WSS TLS ticket (one unstarted session, minted
 * and deleted, about once an hour). Errors here are silent; the next turn
 * reports its own. */
static void idle_upkeep(void)
{
    if (s_state != C6_VOICE_IDLE || !net_wifi_is_connected(NULL, 0)) return;
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    if (!st.paired || st.pairing) return;
    bool ticket_stale = !s_ws_ticket || esp_timer_get_time() - s_ws_ticket_us > TICKET_MAX_AGE_US;
    if (cloud_auth_access_valid_s() >= AUTH_MIN_VALID_S && !ticket_stale) return;
    c6_wake_pause();
    reset_turn_state();
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "idle upkeep: heap %u, largest %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    /* The FIFO is idle while the wake engine is paused: lend it to the
     * account renewal, so only the TLS handshake needs heap. */
    bool lent = FIFO_BYTES >= cloud_auth_work_size() && cloud_auth_set_work_area(s_fifo_storage, FIFO_BYTES);
    esp_err_t auth = load_auth();
    if (lent) {
        cloud_auth_set_work_area(NULL, 0);
        xStreamBufferReset(s_fifo);
        c6_adpcm_reset(&s_enc);
    }
    if (auth == ESP_OK && ticket_stale && mint_session() == ESP_OK) {
        s_warm = true;
        s_accept_downlink = false;
        if (ws_dial() == ESP_OK) {
            for (int i = 0; i < 500 && !s_ws_open && !s_ws_closed; ++i) vTaskDelay(pdMS_TO_TICKS(20));
        }
        ws_close();
        s_warm = false;
        delete_session();
    }
    free_auth();
    ESP_LOGI(TAG, "idle upkeep in %d ms (ticket %s, token %d s, heap %u)",
             (int)((esp_timer_get_time() - t0) / 1000), s_ws_ticket ? "held" : "missing",
             cloud_auth_access_valid_s(), (unsigned)esp_get_free_heap_size());
    reset_turn_state();
    c6_wake_resume();
}

static void voice_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t woke = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000));
        if (take_tap()) run_turn();
        else if (!woke) idle_upkeep();
    }
}

static void load_locale(void)
{
    nvs_handle_t h;
    if (nvs_open("c6_voice", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_locale);
    char tmp[sizeof(s_locale)];
    if (nvs_get_str(h, "locale", tmp, &n) == ESP_OK) strlcpy(s_locale, tmp, sizeof(s_locale));
    nvs_close(h);
}

esp_err_t c6_voice_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    load_locale();
    s_fifo = xStreamBufferCreateStatic(FIFO_BYTES, 1, s_fifo_storage, &s_fifo_struct);
    if (xTaskCreate(capture_task, "c6_capture", 3072, NULL, 6, &s_cap_task) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(voice_task, "c6_voice", 10240, NULL, 5, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

/* Static storage: a heap stream buffer created mid-session split the free
 * heap and starved the next turn's TLS handshake (largest block 80 KB). */
static uint8_t s_inject_storage[4096 + 1];
static StaticStreamBuffer_t s_inject_struct;
static StreamBufferHandle_t s_inject_handle;

static volatile bool s_inject_wake;

StreamBufferHandle_t c6_voice_inject_begin(bool wake)
{
    s_inject_wake = wake;
    if (!s_inject_handle)
        s_inject_handle = xStreamBufferCreateStatic(sizeof(s_inject_storage) - 1, 1, s_inject_storage, &s_inject_struct);
    xStreamBufferReset(s_inject_handle);
    s_inject = s_inject_handle;
    return s_inject;
}

StreamBufferHandle_t c6_voice_inject_stream(void)
{
    return s_inject_wake ? s_inject : NULL;
}

void c6_voice_inject_end(void)
{
    s_inject = NULL;
}

bool c6_voice_server_verifies(void)
{
    return s_server_verifies;
}

void c6_voice_wake(const char *phrase, float score)
{
    if (!s_task) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == C6_VOICE_IDLE) {
        strlcpy(s_wake_phrase, phrase ? phrase : "", sizeof(s_wake_phrase));
        s_wake_score = score;
        s_wake_turn = true;
    }
    /* Leave idle at once so the wake engine stops reading the microphone
     * before the turn's capture starts. */
    if (s_state == C6_VOICE_IDLE) {
        s_state = C6_VOICE_CONNECTING;
        s_tap = true;
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void c6_voice_tap(void)
{
    if (!s_task) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tap = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

c6_voice_state_t c6_voice_state(void)
{
    return s_state;
}

void c6_voice_error(char *out, size_t capacity)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_has_err) kbx_voice_error_short(&s_err, out, capacity);
    else if (capacity) out[0] = '\0';
    xSemaphoreGive(s_lock);
}

bool c6_voice_caption(char *out, size_t capacity, bool *from_agent, uint32_t *seq)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_caption, capacity);
    if (from_agent) *from_agent = s_caption_agent;
    if (seq) *seq = s_caption_seq;
    bool has = s_caption[0] != '\0';
    xSemaphoreGive(s_lock);
    return has;
}

void c6_voice_set_locale(const char *locale)
{
    if (!locale || strlen(locale) >= sizeof(s_locale)) return;
    strlcpy(s_locale, locale, sizeof(s_locale));
    nvs_handle_t h;
    if (nvs_open("c6_voice", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "locale", s_locale);
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *c6_voice_locale(void)
{
    return s_locale;
}

void c6_voice_log_status(void)
{
    char err[48];
    c6_voice_error(err, sizeof(err));
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    ESP_LOGI(TAG, "status: state=%d paired=%d pairing=%d auth_err=%s last_err=%s locale=%s heap=%u largest=%u",
             (int)s_state, st.paired, st.pairing, st.error_code[0] ? st.error_code : "-", err[0] ? err : "-",
             s_locale, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
