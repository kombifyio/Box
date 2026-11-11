#include "c6_voice.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
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
#include "c6_edge.h"
#include "c6_sd.h"
#include "c6_wake.h"
#include "cloud_auth.h"
#include "net_wifi.h"
#include "session_url.h"
#include "voice_control.h"
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
/* Companion route (front door route_switch): its first content takes 13 to
 * 17 s, with pauses between the spoken parts. */
#define COMPANION_THINK_MAX_US ((int64_t)60 * 1000000)
#define COMPANION_REPLY_IDLE_US ((int64_t)30 * 1000000)
/* A spoken "start the Voice Agent" conversation waits longer for the next sentence. */
#define CONVERSATION_NO_SPEECH_US ((int64_t)15 * 1000000)
#define AI_SESSIONS_URL "https://api.kombify.io/v1/ai/sessions"
#define ERROR_SHOW_MS     4000
/* Conversation mode (S3 net_ws.c): after a reply that expects an answer the
 * box listens again on the same session, up to the owner's limit per tap or
 * wake hit. A follow-up stage ends after 6 s without speech; the speaker
 * stays quiet for 350 ms before the microphone reopens. */
#define FOLLOWUP_NO_SPEECH_US ((int64_t)6 * 1000000)
#define FOLLOWUP_ECHO_GUARD_MS 350
#define FOLLOWUPS_MAX     3U
#define FOLLOWUPS_DEFAULT 3U

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
static volatile bool s_verifying, s_wake_rejected, s_wake_verified;
static int64_t s_wake_t0_us;                   /* wake hit */
static volatile int64_t s_verifying_us;       /* server asked for the pre-roll */
static volatile int s_verify_ms;              /* from that request to the verdict */
static bool s_server_verifies;
static volatile bool s_turn_listened;        /* the server answered "listening" this turn (not "verifying") */
static volatile bool s_turn_ws_up;           /* the socket opened this turn (s_ws_open is cleared by ws_close) */

#define AUTH_MIN_VALID_S 150

/* Front door receipts of a plain session (SpeechKit "Device front door"),
 * written by the websocket task, read by the voice task. */
static volatile bool s_conversation;   /* conversation_mode receipt: listen again after each reply */
static volatile bool s_switch_pending; /* route_switch receipt: the Companion takes this turn */
static volatile bool s_agent_turn;     /* the open session is on the Companion route */
static volatile bool s_switch_shown;   /* display: the Companion holds this turn */
static char s_switch_utt[KBX_VOICE_UTTERANCE_MAX + 1U];

/* Written by the websocket task, read by the voice task. */
static volatile bool s_ws_open, s_ws_closed, s_listening, s_input_heard, s_input_final;
static volatile bool s_turn_end, s_output_seen, s_downlink, s_accept_downlink, s_failed;
static volatile int8_t s_expects_reply = -1;   /* turn_end flag: -1 none, 0 no, 1 yes */
static char s_out_tail[8];                     /* last bytes of the agent's text */
static uint8_t s_max_followups = FOLLOWUPS_DEFAULT;
static volatile uint8_t s_stage;               /* 0 = tap or wake turn, 1.. = follow-ups */
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
static char s_resp[6144];   /* headers + body; the edge check-in answers with the device and its settings */
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
    if (esp_transport_ssl_session_ticket_operation(s_ssl, ESP_TRANSPORT_SESSION_TICKET_SAVE) == ESP_OK)
        s_ws_ticket = true;
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

static char s_etag[24];
static int s_poll_after;   /* x-kombify-poll-after of the last response, 0 when absent */

/* extra: complete header lines (each ending in CRLF) or NULL. The response
 * ETag of the last call is kept in s_etag. */
static esp_err_t http_call(const char *method, const char *url, const char *extra, const char *body, int *status)
{
    *status = 0;
    s_resp[0] = 0;
    s_etag[0] = 0;
    s_poll_after = 0;
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
              tls_write("\r\n", 2) && (!extra || tls_write(extra, strlen(extra))) && tls_write("\r\n", 2) &&
              (!body_len || tls_write(body, body_len));
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
    const char *poll = header_value(s_resp, "x-kombify-poll-after");
    if (poll) s_poll_after = atoi(poll);
    const char *etag = header_value(s_resp, "ETag");
    if (etag) {
        size_t e = strcspn(etag, "\r\n");
        if (e < sizeof(s_etag)) {
            memcpy(s_etag, etag, e);
            s_etag[e] = 0;
        }
    }
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

/* A non-2xx Gateway answer in s_resp becomes the turn's failure. */
static void refused(const char *what, int status)
{
    kbx_voice_error_t e;
    kbx_voice_error_from_http(status, s_resp, &e);
    ESP_LOGW(TAG, "%s refused: http %d code=%s reason=%s request_id=%s", what, status, e.code,
             e.reason[0] ? e.reason : "-", e.request_id[0] ? e.request_id : "-");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_has_err) { s_err = e; s_has_err = true; }
    s_failed = true;
    xSemaphoreGive(s_lock);
}

/* fields: the body members without braces, "" for the quick route, the
 * registered-agent members for the Companion. The board's edge device id is
 * added, so the session counts against this board, never against the
 * account's other boards. */
static esp_err_t mint_session(const char *fields)
{
    static char body[448]; /* voice task only; kept off its stack */
    if (!kbx_session_url(KBX_CLOUD_SERVER, NULL, false, s_http_url, sizeof(s_http_url))) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "server_url");
        return ESP_ERR_INVALID_ARG;
    }
    /* Only the id of this mint's own account, once a check-in confirmed it
     * (it uses s_resp as scratch, before the call below fills it). */
    char device[64];
    (void)c6_edge_mint_device_id(s_auth, device, sizeof(device));
    if (!kbx_mint_body(fields, device, body, sizeof(body))) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "session_body");
        return ESP_ERR_INVALID_SIZE;
    }
    int status = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = http_call("POST", s_http_url, NULL, body, &status);
    if (err != ESP_OK) {
        fail(net_wifi_is_connected(NULL, 0) ? KBX_VOICE_ERR_SERVICE : KBX_VOICE_ERR_OFFLINE, 0,
             err == ESP_ERR_INVALID_SIZE ? "response_too_large" : "connect");
        return err;
    }
    ESP_LOGI(TAG, "session-create http %d in %d ms", status, (int)((esp_timer_get_time() - t0) / 1000));
    if (status < 200 || status >= 300) {
        refused("session-create", status);
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

/* The Companion session of a route switch: one Assist history, then the
 * registered kombify-agent session bound to it (S3 net_ws.c, agent route). */
static esp_err_t mint_companion(void)
{
    static const char history[] = "{\"title\":\"New chat\",\"metadata\":{"
        "\"harnessId\":\"kombify-ai\",\"historyKind\":\"assist\","
        "\"speechkit\":{\"surface\":\"box\",\"voiceMode\":\"oneshot\"}}}";
    char key[48];
    snprintf(key, sizeof(key), "Idempotency-Key: c6-%08x%08x\r\n", (unsigned)esp_random(), (unsigned)esp_random());
    int status = 0;
    esp_err_t err = http_call("POST", AI_SESSIONS_URL, key, history, &status);
    if (err != ESP_OK) {
        fail(net_wifi_is_connected(NULL, 0) ? KBX_VOICE_ERR_SERVICE : KBX_VOICE_ERR_OFFLINE, 0, "connect");
        return err;
    }
    if (status < 200 || status >= 300) {
        refused("history-create", status);
        return ESP_FAIL;
    }
    char body[320];
    cJSON *root = cJSON_Parse(s_resp);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    int n = kbx_voice_control_id(id) ? snprintf(body, sizeof(body),
        "\"provider\":\"kombify-agent\",\"target_agent_id\":\"workbench-orchestrator\","
        "\"ai_session_id\":\"%s\",\"harness_id\":\"kombify-ai\",\"composer_surface\":\"box\"",
        id->valuestring) : -1;
    cJSON_Delete(root);
    wipe(s_resp, sizeof(s_resp));
    if (n < 0 || n >= (int)sizeof(body)) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "bad_history_response");
        return ESP_FAIL;
    }
    return mint_session(body);
}

static void delete_session(void)
{
    if (!s_sid[0] || !s_auth) return;
    if (kbx_session_url(KBX_CLOUD_SERVER, s_sid, false, s_http_url, sizeof(s_http_url))) {
        int status = 0;
        if (http_call("DELETE", s_http_url, NULL, NULL, &status) == ESP_OK)
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
    if (s_switch_pending) {
        /* The Companion takes this turn: nothing more of this session counts. */
        cJSON_Delete(root);
        return;
    }
    /* Typed receipts of THIS session only (never spoken or generated text):
     * conversation_mode keeps the box listening after each reply, route_switch
     * hands the turn to the Companion. */
    kbx_voice_control_t control;
    if (kbx_voice_control(root, s_sid, &control)) {
        if (control.accepted) {
            s_conversation = control.mode == KBX_VOICE_MODE_CONVERSATION;
            ESP_LOGI(TAG, "authenticated conversation mode=%s", s_conversation ? "conversation" : "one-shot");
        }
    } else if (!s_agent_turn) {
        kbx_voice_route_switch_t sw;
        if (kbx_voice_route_switch(root, s_sid, &sw)) {
            memcpy(s_switch_utt, sw.utterance, strlen(sw.utterance) + 1U);
            wipe(&sw, sizeof(sw));
            ESP_LOGI(TAG, "route switch to the Companion");
            s_conversation = false;
            s_accept_downlink = false;
            s_switch_shown = true;
            s_switch_pending = true;
            cJSON_Delete(root);
            return;
        }
    }
    unsigned phases = kbx_voice_phases(root);
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(jt) ? jt->valuestring : "";
    const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *jer = cJSON_GetObjectItemCaseSensitive(root, "expects_reply");
    ESP_LOGI(TAG, "rx %s%s%s%s%s len=%u", type, cJSON_IsString(js) ? " state=" : "",
             cJSON_IsString(js) ? js->valuestring : "",
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "done")) ? " done" : "",
             cJSON_IsBool(jer) ? (cJSON_IsTrue(jer) ? " expects_reply" : " no_reply") : "", (unsigned)length);
    if (cJSON_IsBool(jer)) s_expects_reply = cJSON_IsTrue(jer) ? 1 : 0;
    if (!strcmp(type, "state") && cJSON_IsString(js) && !strcmp(js->valuestring, "verifying")) {
        /* Two-stage wake word: the server wants the pre-roll and speech now. */
        s_verifying = true;
        s_listening = true;
        s_verifying_us = esp_timer_get_time();
    } else if (!strcmp(type, "wake_verified")) {
        s_verify_ms = (int)((esp_timer_get_time() - s_verifying_us) / 1000);
        s_wake_verified = true;
        ESP_LOGI(TAG, "wake word verified by the server: %d ms after the pre-roll request, %d ms after the hit",
                 s_verify_ms, (int)((esp_timer_get_time() - s_wake_t0_us) / 1000));
    } else if (!strcmp(type, "wake_rejected")) {
        s_verify_ms = (int)((esp_timer_get_time() - s_verifying_us) / 1000);
        ESP_LOGI(TAG, "wake word rejected by the server: %d ms after the pre-roll request, %d ms after the hit",
                 s_verify_ms, (int)((esp_timer_get_time() - s_wake_t0_us) / 1000));
        s_wake_rejected = true;
    } else if (!strcmp(type, "state") && cJSON_IsString(js) && !strcmp(js->valuestring, "listening")) {
        s_listening = true;
        s_turn_listened = true;
    } else if (!strcmp(type, "error")) {
        const cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "code");
        const char *code = cJSON_IsString(jc) ? jc->valuestring : "server_error";
        const cJSON *jk = cJSON_GetObjectItemCaseSensitive(root, "class");
        /* SpeechKit names the reason class of every in-band error. */
        fail(kbx_voice_error_from_class(cJSON_IsString(jk) ? jk->valuestring : NULL, code), 0, code);
        /* The device's conversation cap comes with a full sentence in the
         * start frame's language (de, en, es): show it as the caption. */
        const cJSON *jm = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (strstr(code, "device_limit_exceeded") && cJSON_IsString(jm) && jm->valuestring[0] &&
            kbx_voice_utf8_valid(jm->valuestring) &&
            (!strncmp(s_locale, "de", 2) || !strncmp(s_locale, "en", 2) || !strncmp(s_locale, "es", 2)))
            set_caption(jm->valuestring, true);
    } else if (!strcmp(type, "input_transcript") || !strcmp(type, "output_transcript")) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) {
            set_caption(text->valuestring, type[0] == 'o');
            if (type[0] == 'o') {
                /* Tail of the agent's text: the fallback for a server that
                 * sends no expects_reply (cumulative text or deltas, the
                 * last fragment ends the text either way). */
                const char *t = text->valuestring;
                size_t n = strlen(t);
                while (n && isspace((unsigned char)t[n - 1])) --n;
                if (n) {
                    size_t k = n < sizeof(s_out_tail) - 1 ? n : sizeof(s_out_tail) - 1;
                    memcpy(s_out_tail, t + n - k, k);
                    s_out_tail[k] = 0;
                }
            }
        }
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
        s_turn_ws_up = true;
        ESP_LOGI(TAG, "ws dial %d ms (%s)", (int)((esp_timer_get_time() - s_ws_dial_us) / 1000),
                 s_ws_ticket ? "resumed" : "full handshake");
        tls_ticket_save();
        char start[320];
        char wake[96] = "";
        if (s_wake_turn && !s_agent_turn)
            snprintf(wake, sizeof(wake), ",\"wake\":{\"phrase\":\"%s\",\"score\":%.2f,\"preroll_ms\":%d}",
                     s_wake_phrase, (double)s_wake_score, PREROLL_FRAMES * 20);
        /* The quick session opts into the front door (spoken mode commands and
         * Companion requests, answered with receipts; an older server ignores
         * it); the Companion session is a registered agent with its own control. */
        const char *door = s_agent_turn ? ",\"conversation_control\":{\"version\":1,\"mode\":\"oneshot\"}" :
            ",\"front_door\":{\"version\":1,\"routes\":[\"agent\"]}";
        snprintf(start, sizeof(start),
                 "{\"type\":\"start\",\"media_transport\":\"websocket\",\"locale\":\"%s\","
                 "\"activity_detection\":{\"automatic\":true}%s%s}", s_locale, door, wake);
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
        if (cancelled() || s_switch_pending) break;
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
        if (now - s_last_downlink_us > (s_agent_turn ? COMPANION_REPLY_IDLE_US : REPLY_IDLE_US) ||
            now - start > REPLY_MAX_US) {
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
            StreamBufferHandle_t inject = s_inject;
            if (inject) {
                /* Operator test input (KBX inject) replaces the microphone
                 * samples; silence follows once the clip ends. */
                size_t got = 0;
                /* Whole frames: a short read padded with zeros would chop the clip. */
                for (int i = 0; i < 10 && got < sizeof(pcm); ++i)
                    got += xStreamBufferReceive(inject, (uint8_t *)pcm + got, sizeof(pcm) - got, pdMS_TO_TICKS(20));
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

static void capture_start(bool keep_preroll)
{
    /* A wake turn keeps the pre-roll the engine left in the FIFO. */
    if (!keep_preroll) {
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
    /* An injected clip read takes up to 200 ms; the task must be out of its
     * loop before anything else may read the same stream. */
    for (int i = 0; i < 100 && !s_capture_idle; ++i) vTaskDelay(pdMS_TO_TICKS(5));
}

/* ── One turn ─────────────────────────────────────────────────────────── */

static void reset_turn_state(void)
{
    s_ws_open = s_ws_closed = s_listening = s_input_heard = s_input_final = false;
    s_turn_end = s_output_seen = s_downlink = s_failed = false;
    s_verifying = s_wake_rejected = s_wake_verified = s_turn_ws_up = s_turn_listened = false;
    s_verify_ms = 0;
    s_accept_downlink = true;
    s_downlink_bytes = 0;
    s_ws_http_status = 0;
    s_last_opcode = 0;
    s_expects_reply = -1;
    s_out_tail[0] = 0;
    s_stage = 0;
    s_conversation = s_switch_pending = s_agent_turn = s_switch_shown = false;
    s_switch_utt[0] = 0;
    kbx_voice_text_reset(&s_text);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_has_err = false;
    memset(&s_err, 0, sizeof(s_err));
    xSemaphoreGive(s_lock);
}

static bool tail_is_question(const char *t)
{
    size_t n = strlen(t);
    if (n >= 1 && t[n - 1] == '?') return true;
    /* Fullwidth U+FF1F and Arabic U+061F. */
    if (n >= 3 && (uint8_t)t[n - 3] == 0xEF && (uint8_t)t[n - 2] == 0xBC && (uint8_t)t[n - 1] == 0x9F) return true;
    if (n >= 2 && (uint8_t)t[n - 2] == 0xD8 && (uint8_t)t[n - 1] == 0x9F) return true;
    return false;
}

/* The server's expects_reply flag decides; without it a reply ending in a
 * question does (S3: net_ws.c followup_wanted_locked). A conversation (spoken
 * "start the Voice Agent") always listens again. */
static bool followup_wanted(void)
{
    if (s_conversation) return true;
    if (s_expects_reply >= 0) return s_expects_reply == 1;
    return tail_is_question(s_out_tail);
}

/* The next user turn on the open session: fresh per-turn flags, the
 * speaker's tail settles, then the microphone and the capture FIFO restart. */
static bool followup_begin(void)
{
    s_input_heard = s_input_final = false;
    s_turn_end = s_output_seen = s_downlink = false;
    s_downlink_bytes = 0;
    s_last_opcode = 0;
    s_expects_reply = -1;
    s_out_tail[0] = 0;
    set_caption("", false);
    vTaskDelay(pdMS_TO_TICKS(FOLLOWUP_ECHO_GUARD_MS));
    if (c6_audio_mic_open() != ESP_OK) {
        fail(KBX_VOICE_ERR_UNKNOWN, 0, "microphone");
        return false;
    }
    capture_start(false);
    return true;
}

/* Wait for the reply of the open session and play it. True when the caller
 * may start another stage (the reply asks for an answer, or a conversation
 * runs). A route_switch receipt ends the wait: the caller opens the Companion. */
static bool reply_phase(unsigned stage)
{
    set_state(C6_VOICE_THINKING);
    int64_t think_start = esp_timer_get_time();
    const int64_t think_max = s_agent_turn ? COMPANION_THINK_MAX_US : THINK_MAX_US;
    while (!s_downlink) {
        if (cancelled() || s_failed || s_switch_pending) return false;
        /* A turn_end left over from the previous reply must not end this one. */
        if (s_ws_closed || (s_turn_end && (!stage || s_input_heard || s_input_final))) break;
        if (esp_timer_get_time() - think_start > think_max) {
            fail(KBX_VOICE_ERR_SERVICE, 0, "reply_timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_downlink) play_reply();
    if (s_failed || s_ws_closed || s_switch_pending) return false;
    return followup_wanted();
}

/* One user turn on the open session: uplink until the server heard the
 * sentence, wait for the reply and play it. True when the reply asks for an
 * answer and the caller may start the next stage. Errors are recorded
 * through fail(). */
static bool run_stage(unsigned stage)
{
    /* A wake turn keeps "connecting" until the server verified the phrase:
     * the transcription provider connects only after wake_verified, so an
     * earlier "listening" shows a session that cannot yet hear. A server
     * that does not verify at all never sets s_verifying, so its wake turns
     * listen at once. */
    bool wake_pending = s_wake_turn && !stage && s_verifying && !s_wake_verified && !s_wake_rejected;
    bool listening_shown = !wake_pending;
    if (!wake_pending) set_state(C6_VOICE_LISTENING);
    int64_t listen_start = esp_timer_get_time();
    ESP_LOGI(TAG, "stage %u: heap %u, largest %u, min %u", stage, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)esp_get_minimum_free_heap_size());
    int64_t no_speech_us = !stage ? NO_SPEECH_US : s_conversation ? CONVERSATION_NO_SPEECH_US : FOLLOWUP_NO_SPEECH_US;
    uint32_t frames = 0;
    bool silent = false;
    for (;;) {
        if (!listening_shown && (s_wake_verified || s_wake_rejected)) {
            set_state(C6_VOICE_LISTENING);
            listening_shown = true;
        }
        if (take_tap()) {
            ESP_LOGI(TAG, "tap ends the utterance");
            break;
        }
        if (s_input_final || s_downlink || s_failed || s_ws_closed || s_switch_pending) break;
        int64_t now = esp_timer_get_time();
        if (now - listen_start > LISTEN_MAX_US) break;
        if (!s_input_heard && now - listen_start > no_speech_us) {
            silent = true;
            break;
        }
        size_t got = xStreamBufferReceive(s_fifo, s_adpcm, sizeof(s_adpcm), pdMS_TO_TICKS(100));
        if (got == 0) continue;
        for (int i = 0; i < 5 && got < sizeof(s_adpcm); ++i)
            got += xStreamBufferReceive(s_fifo, s_adpcm + got, sizeof(s_adpcm) - got, pdMS_TO_TICKS(20));
        if (got < sizeof(s_adpcm)) continue;
        fifo_decode(s_adpcm, s_frame);
        if (s_wake_turn && !stage) c6_sd_wake_frame(s_frame, FRAME_SAMPLES);
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
    if (s_switch_pending) return false;   /* run_turn hands the turn to the Companion */
    if (silent) {
        /* Silence after a follow-up prompt just ends the conversation. */
        if (!stage) fail(KBX_VOICE_ERR_NONE, 0, "no_speech");
        else ESP_LOGI(TAG, "follow-up: no speech, conversation ends");
        return false;
    }
    if (s_failed || s_ws_closed) return false;
    if (!s_downlink) send_text("{\"type\":\"audio_end\"}");
    return reply_phase(stage);
}

/* Waits for the server's listening acknowledgement of the dialed session. The
 * reply buffer exists only once the socket is up, so the account refresh and
 * the TLS handshakes before it keep 32 KB more. */
static bool wait_listening(int64_t t0)
{
    while (!s_listening) {
        if (s_ws_open && !s_ring && !(s_ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_BYTEBUF))) {
            fail(KBX_VOICE_ERR_UNKNOWN, 0, "no_memory");
            return false;
        }
        if (cancelled() || s_failed) return false;
        if (s_ws_closed || esp_timer_get_time() - t0 > CONNECT_MAX_US) {
            fail(KBX_VOICE_ERR_SERVICE, s_ws_http_status, s_ws_http_status ? "ws_upgrade" : "connect_timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

/* A route_switch receipt: leave the quick session, serve THIS turn on the
 * Companion route (the saved settings are untouched), replay the utterance as
 * the first text frame and play the answer. The next wake or tap starts on
 * the quick route again. The ring buffer goes first, as at a turn start, so
 * the Companion's TLS handshake has the heap of a fresh turn. True when the
 * Companion's answer asks for another stage. */
static bool run_companion(void)
{
    ESP_LOGI(TAG, "route switch: closing the quick session (heap %u, largest %u)", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_switch_pending = false;
    set_state(C6_VOICE_THINKING);
    ws_close();
    if (s_ring) {
        vRingbufferDelete(s_ring);
        s_ring = NULL;
    }
    delete_session();
    s_ws_open = s_ws_closed = s_listening = false;
    s_turn_end = s_output_seen = s_downlink = false;
    s_downlink_bytes = 0;
    s_ws_http_status = 0;
    s_last_opcode = 0;
    s_expects_reply = -1;
    s_out_tail[0] = 0;
    s_accept_downlink = true;
    /* The words are already in the utterance: nothing more goes up until the
     * answer asks for a reply. */
    s_input_heard = s_input_final = true;
    kbx_voice_text_reset(&s_text);
    s_agent_turn = true;
    int64_t t0 = esp_timer_get_time();
    if (mint_companion() != ESP_OK || cancelled()) return false;
    if (ws_dial() != ESP_OK || !wait_listening(t0)) return false;
    ESP_LOGI(TAG, "Companion session listening after %d ms (heap %u, largest %u)",
             (int)((esp_timer_get_time() - t0) / 1000), (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    bool sent = kbx_voice_text_frame(s_switch_utt, s_resp, sizeof(s_resp)) && s_ws && s_ws_open &&
        esp_websocket_client_send_text(s_ws, s_resp, (int)strlen(s_resp), pdMS_TO_TICKS(2000)) == (int)strlen(s_resp);
    wipe(s_resp, sizeof(s_resp));
    wipe(s_switch_utt, sizeof(s_switch_utt));
    if (!sent) {
        fail(KBX_VOICE_ERR_SERVICE, 0, "text_send");
        return false;
    }
    return reply_phase(0);
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
    if (s_wake_turn) c6_sd_wake_begin(s_wake_phrase, s_wake_score);
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
    capture_start(s_wake_turn);
    if (load_auth() != ESP_OK || cancelled()) goto done;
    if (mint_session("") != ESP_OK || cancelled()) goto done;
    if (ws_dial() != ESP_OK) goto done;
    if (!wait_listening(t0)) goto done;
    size_t backlog = xStreamBufferBytesAvailable(s_fifo);
    ESP_LOGI(TAG, "listening after %d ms, %u ms of speech buffered (%u frames dropped)",
             (int)((esp_timer_get_time() - t0) / 1000), (unsigned)(backlog / ADPCM_FRAME * 20U), (unsigned)s_capture_drops);

    for (unsigned left = s_max_followups;; --left) {
        bool again = run_stage(s_stage);
        if (s_switch_pending && !s_failed) again = run_companion();
        /* A conversation has no stage limit; it ends on silence, an
         * "end the conversation" receipt or a tap. */
        if (!again || (left == 0 && !s_conversation)) break;
        /* The reply asked for an answer: another user turn on this session. */
        s_stage++;
        ESP_LOGI(TAG, "follow-up stage %u/%u listening (%s)", (unsigned)s_stage, (unsigned)s_max_followups,
                 s_conversation ? "conversation" : s_expects_reply >= 0 ? "server expects_reply" : "reply ends with a question");
        if (!followup_begin()) break;
    }

done:
    s_switch_shown = false;
    capture_stop();
    c6_audio_mic_close();
    s_inject = NULL;   /* the test stream lives for one conversation */
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
    /* s_ws_open is already cleared by ws_close(); the first version of this
     * check read it and so never learned that the server verifies, which kept
     * the low candidate cutoff from ever engaging. */
    /* Learn from "verifying". Unlearn only from a turn the server accepted
     * ("listening") without ever asking for the pre-roll and that ended
     * without an error: a 503, a limit, budget or auth error, a dropped socket
     * or session_unavailable says nothing about the server's behaviour. */
    bool learn = s_verifying && !s_server_verifies;
    bool unlearn = !s_verifying && s_server_verifies && s_turn_listened && !s_has_err;
    if (s_wake_turn && s_turn_ws_up && (learn || unlearn)) {
        s_server_verifies = s_verifying;
        ESP_LOGI(TAG, "server %s wake words", s_server_verifies ? "verifies" : "does not verify");
        nvs_handle_t vh;
        if (nvs_open("c6_voice", NVS_READWRITE, &vh) == ESP_OK) {
            nvs_set_u8(vh, "verifies", s_server_verifies);
            nvs_commit(vh);
            nvs_close(vh);
        }
    }
    /* A rejected candidate is the expected outcome of a low device cutoff:
     * back to idle without an error. */
    bool show_error = s_has_err && strcmp(s_err.code, "cancelled") != 0 && !s_wake_rejected;
    if (s_wake_turn) {
        c6_sd_wake_end(s_wake_rejected ? "rejected" : s_wake_verified ? "accepted" : s_verifying ? "failed" : s_has_err ? "failed" : "local",
                       s_verify_ms);
    }
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

/* Keeps the first spoken turn fast: renew the account token before it runs
 * out. The edge check-in and settings poll (at least every 5 min) refresh
 * the TLS ticket the voice session resumes, so no voice session is minted
 * here: every mint reserves up to four hours of the account's voice minutes.
 * Errors here are silent; the next turn reports its own. */
static void idle_upkeep(void)
{
    if (s_state != C6_VOICE_IDLE || !net_wifi_is_connected(NULL, 0)) return;
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    if (!st.paired || st.pairing) return;
    bool edge_due = c6_edge_due();
    if (cloud_auth_access_valid_s() >= AUTH_MIN_VALID_S && !edge_due) return;
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
    /* Edge-device check-in and settings poll ride on the same TLS transport;
     * every Gateway response refreshes the resumption ticket. */
    if (auth == ESP_OK && edge_due) c6_edge_run(s_auth + strlen("Bearer "));
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
        uint32_t woke = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(c6_edge_wait_ms(15000)));
        if (take_tap()) run_turn();
        else if (!woke) idle_upkeep();
    }
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("c6_voice", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_locale);
    char tmp[sizeof(s_locale)];
    if (nvs_get_str(h, "locale", tmp, &n) == ESP_OK) strlcpy(s_locale, tmp, sizeof(s_locale));
    uint8_t v = FOLLOWUPS_DEFAULT;
    if (nvs_get_u8(h, "maxfu", &v) == ESP_OK && v <= FOLLOWUPS_MAX) s_max_followups = v;
    uint8_t ver = 0;
    if (nvs_get_u8(h, "verifies", &ver) == ESP_OK) s_server_verifies = ver != 0;
    nvs_close(h);
}

esp_err_t c6_voice_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    load_settings();
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
    /* One reader at a time: a second task blocked in xStreamBufferReceive
     * on the same stream trips FreeRTOS's assert (coredump 2026-10-22: the
     * capture task read the clip while the resumed wake task did too). */
    return s_inject_wake && s_capture_idle && !s_capture ? s_inject : NULL;
}

void c6_voice_inject_end(void)
{
    s_inject = NULL;
}

StreamBufferHandle_t c6_voice_inject_stream_active(void)
{
    return s_inject;
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
        s_wake_t0_us = esp_timer_get_time();
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

bool c6_voice_companion(void)
{
    return s_switch_shown && s_state != C6_VOICE_IDLE;
}

/* Test console (KBX frontdoor ...): a receipt for the running session through
 * the socket's own handler, so the front-door handling can be proven before
 * the server sends it. Only while a session is open. */
bool c6_voice_debug_receipt(const char *kind, const char *arg)
{
    if (!kind || !arg || !arg[0] || !s_sid[0] || !s_ws_open) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    char request_id[40];
    snprintf(request_id, sizeof(request_id), "c6-%08x", (unsigned)esp_random());
    bool ok = true;
    cJSON_AddStringToObject(root, "type", "event");
    if (!strcmp(kind, "route")) {
        cJSON_AddStringToObject(root, "event_type", "route_switch");
        cJSON *route = cJSON_AddObjectToObject(root, "route");
        cJSON_AddNumberToObject(route, "version", 1);
        cJSON_AddStringToObject(route, "to", "agent");
        cJSON_AddStringToObject(route, "voice_session_id", s_sid);
        cJSON_AddStringToObject(route, "request_id", request_id);
        cJSON_AddStringToObject(route, "reason", "explicit_companion");
        cJSON_AddStringToObject(route, "utterance", arg);
    } else if (!strcmp(kind, "mode")) {
        cJSON_AddStringToObject(root, "event_type", "conversation_mode");
        cJSON *receipt = cJSON_AddObjectToObject(root, "conversation");
        cJSON_AddNumberToObject(receipt, "version", 1);
        cJSON_AddStringToObject(receipt, "mode", arg);
        cJSON_AddStringToObject(receipt, "status", "accepted");
        cJSON_AddStringToObject(receipt, "voice_session_id", s_sid);
        cJSON_AddStringToObject(receipt, "conversation_session_id", s_sid);
        cJSON_AddStringToObject(receipt, "request_id", request_id);
    } else {
        ok = false;
    }
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    const bool fed = text != NULL;
    if (fed) handle_text(text, strlen(text));
    free(text);
    ESP_LOGI(TAG, "test receipt %s: %s", kind, fed ? "fed" : "ignored");
    return fed;
}

kbx_voice_error_kind_t c6_voice_error_kind(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    kbx_voice_error_kind_t kind = s_has_err ? s_err.kind : KBX_VOICE_ERR_NONE;
    xSemaphoreGive(s_lock);
    return kind;
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

esp_err_t c6_voice_set_max_followups(unsigned n)
{
    if (n > FOLLOWUPS_MAX) return ESP_ERR_INVALID_ARG;
    s_max_followups = (uint8_t)n;
    nvs_handle_t h;
    if (nvs_open("c6_voice", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "maxfu", (uint8_t)n);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

unsigned c6_voice_max_followups(void)
{
    return s_max_followups;
}

unsigned c6_voice_stage(void)
{
    return s_stage;
}

int c6_voice_gateway_call(const char *method, const char *url, const char *extra, const char *body,
                          const char **resp, const char **etag, int *poll_after)
{
    int status = 0;
    esp_err_t err = http_call(method, url, extra, body, &status);
    *resp = s_resp;
    *etag = s_etag;
    *poll_after = s_poll_after;
    return err == ESP_OK ? status : 0;
}

char *c6_voice_scratch(size_t *capacity)
{
    *capacity = sizeof(s_resp);
    return s_resp;
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
