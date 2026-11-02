/*
 * kombify box - net_ws: standalone voice-agent client (M1: handshake).
 *
 * Lifecycle task opens a bounded conversation only on an explicit Talk tap.
 * Agent-on leaves identity and Wi-Fi armed without a provider. POST/DELETE run
 * ONLY in net_ws_task (never in the CDC/LVGL task); the WS event callback only
 * sets flags and sends the start frame. Audio (mic 16 kHz up / 24 kHz down)
 * arrives in M1.3/M1.4 — here it ends at state:listening.
 *
 * Pre-flash review fixes (2026-07-13):
 *  - large HTTP buffers are file-static (only net_ws_task uses them, serially)
 *    instead of on the stack so the TLS handshake has stack room (task 12288).
 *  - Token/ticket/response sized large (2048) + truncation is detected and
 *    aborts instead of silently producing broken JSON/subprotocol.
 *  - Server session is deleted on ALL post-create error paths (no leak).
 *  - Re-provisioning (server/token/clear) is staged and applied only AFTER
 *    teardown (with the old creds -> clean DELETE).
 *  - s_lock NULL guards for the boot-window race; s_session_id lock-protected.
 */
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps (PSRAM-Stack) */
#include "freertos/semphr.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "nvs.h"

#include "audio.h"
#include "cloud_auth.h"
#include "box_status.h"
#include "i18n.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "session_url.h"
#include "voice_buffer.h"
#include "voice_control.h"
#include "voice_error.h"
#include "voice_protocol.h"
#include "voice_warm.h"
#include "wakeword.h"

/* overlays/esp_websocket_client.c (KOMBIFY-PATCH): lock-free writability
 * probe of the connected socket; >0 writable, 0 not now, <0 error. */
int esp_websocket_client_kbx_writable(esp_websocket_client_handle_t client, int timeout_ms);

static const char *TAG = "net_ws";

/* Front door receipts (conversation_mode, route_switch) count for a Cloud
 * session only. Dev bench builds (CONFIG_KBX_DEV_ENDPOINTS, never a release)
 * also accept them from a self-hosted server, so a PC mock of the session
 * protocol can prove the mode switch and the Companion hand-over on hardware
 * while the production service is not reachable. */
#if CONFIG_KBX_DEV_ENDPOINTS
#define FRONT_DOOR_SESSION() true
#else
#define FRONT_DOOR_SESSION() s_cloud_session
#endif

#define NET_NVS_NAMESPACE "net"
#define NET_NVS_KEY_URL "sk_url"
#define NET_NVS_KEY_TOKEN "sk_token"
#define NET_NVS_KEY_PROVIDER "sk_provider"
#define NET_NVS_KEY_AGENT "sk_agent"
#define NET_NVS_KEY_ROUTE "sk_route"

/* Provider for the start frame. EMPTY = server default (recommended; on
 * speechkit.kombify.io that is gemini) — switchable via NVS/CDC
 * (KBX provider <name|clear>). A set name must match the server provider
 * registry, otherwise the server replies with error/provider_unavailable
 * (see handle_ws_text). Evidence 2026-08-22: deepgram currently yields no
 * reply audio on prod, gemini does — hence server default instead of a
 * hard-coded deepgram. Downlink is 24 kHz across providers
 * (LiveMessage.Audio contract). */
#define NET_WS_PROVIDER_MAX 32
static char s_provider[NET_WS_PROVIDER_MAX] = ""; /* under s_lock */

/* Production account claims currently produce 4,758-byte JWTs. Keep the
 * account, HTTP header and cleanup credential capacities identical. */
#define NET_WS_TOKEN_MAX CLOUD_AUTH_TOKEN_MAX
#define NET_WS_URL_MAX 256

/* Provisioning (under s_lock against the CDC task). */
static SemaphoreHandle_t s_lock = NULL;
static EXT_RAM_BSS_ATTR char s_url[NET_WS_URL_MAX]; /* base URL, no trailing slash */
static EXT_RAM_BSS_ATTR char s_token[NET_WS_TOKEN_MAX]; /* Bearer; NEVER logged */

/* Provisioning changes cancel the explicit request. Cleanup remains bound
 * to the endpoint and credential used for the original session mint. */

/* Observability: the CDC task reads this in net_ws_log_status to see WHETHER
 * the lifecycle task is running at all (s_loops counts up) and how much
 * internal RAM is free (task-stack alloc otherwise fails silently at boot). */
static volatile uint32_t s_loops = 0;
static bool s_task_started = false;

/* Session held by the lifecycle task. s_session_id is lock-protected (the
 * CDC task reads it in net_ws_log_status). */
static volatile bool s_agent_enabled = false;
static volatile bool s_suspended = false;
static volatile bool s_warm_hold = false; /* edge sync needs the warm path's RAM */
static esp_websocket_client_handle_t s_client = NULL;
static char s_session_id[64] = "";
static EXT_RAM_BSS_ATTR char s_ws_url[NET_WS_URL_MAX];
static EXT_RAM_BSS_ATTR char s_ws_subproto[512];
static volatile bool s_ws_connected = false;
static volatile bool s_ws_closed = false;
static volatile bool s_active = false;
static volatile bool s_transport_idle = true; /* no create, WS or DELETE */
static int64_t s_connect_deadline_us = 0;
static uint32_t s_heap_at_open = 0;

/* HTTP scratch: ONLY net_ws_task uses them, always serially (create OR
 * delete, never both) -> file-static instead of stack so the mbedTLS
 * handshake does not compete with ~2.8 KB of locals for stack. */
static EXT_RAM_BSS_ATTR char s_http_url[NET_WS_URL_MAX + 96];
static EXT_RAM_BSS_ATTR char s_http_auth[NET_WS_TOKEN_MAX + 16];
static EXT_RAM_BSS_ATTR char s_http_resp[NET_WS_TOKEN_MAX];
/* Bind cleanup and the ticket transport to the endpoint actually used at mint,
 * even when provisioning changes while a TLS request is in flight. */
static EXT_RAM_BSS_ATTR char s_session_base[NET_WS_URL_MAX];
static EXT_RAM_BSS_ATTR char s_request_token[NET_WS_TOKEN_MAX];
static uint32_t s_revision;
static uint32_t s_session_revision;
static bool s_cloud_session;
/* Voice route for a cloud-paired box. Fast (default): the direct SpeechKit
 * voice agent, about 1 s to a spoken reply. Agent: the governed Workbench
 * orchestrator with Assist history, currently too slow for a speaker
 * (17 s and more per turn); selectable with KBX route agent. */
static bool s_route_agent;
static bool s_session_agent; /* route the current session was minted for */
static EXT_RAM_BSS_ATTR char s_assist_session_id[KBX_VOICE_CONTROL_ID_MAX + 1U];
static EXT_RAM_BSS_ATTR char s_http_body[512];
/* Credential that minted the outstanding (prepared or active) session; its
 * DELETE uses it even after a local logout. Wiped once the session is gone. */
static EXT_RAM_BSS_ATTR char s_session_auth[NET_WS_TOKEN_MAX + 16];
static char s_ticket_expires[48];

/* ── Warm path (net_ws_task only unless noted) ───────────────────────────────
 * 1. One persistent keep-alive HTTPS client to the API host: history, mint
 *    and DELETE reuse one TLS connection instead of a handshake per call.
 * 2. One canonical Assist history created ahead of time (no budget); each
 *    tap consumes it and the idle loop prepares the next one.
 * 3. At most one prepared voice session (ticket) in the follow-up window
 *    after a finished turn or a fresh pairing. It reserves budget but bills
 *    nothing unless started; it is deleted before its ticket expires
 *    (voice_warm.h) and never re-minted without another turn. */
#define AI_SESSIONS_URL "https://api.kombify.io/v1/ai/sessions"
#define WARM_RETRY_MIN_US ((int64_t)30 * 1000 * 1000)
#define WARM_RETRY_MAX_US ((int64_t)600 * 1000 * 1000)
static esp_http_client_handle_t s_api;
static volatile bool s_api_open;
static bool s_api_warm;      /* last response left the connection reusable */
static char s_api_host[96];
static volatile uint32_t s_prov_epoch; /* bumped under s_lock on any identity/agent change */
static EXT_RAM_BSS_ATTR char s_warm_history[KBX_VOICE_CONTROL_ID_MAX + 1U];
static uint32_t s_warm_history_epoch;
static int64_t s_warm_retry_us;
static uint32_t s_warm_failures;
static char s_warm_key[48];  /* Idempotency-Key until that history exists */
static volatile bool s_prepared;
static int64_t s_prepared_until_us;
static uint32_t s_prepared_epoch;
static EXT_RAM_BSS_ATTR char s_prepared_history[KBX_VOICE_CONTROL_ID_MAX + 1U];
/* Follow-up window: a turn just finished or pairing just completed. */
#define PREPARE_WINDOW_US ((int64_t)30 * 1000 * 1000)
static int64_t s_prepare_wanted_until_us;
static volatile bool s_bg_busy;
static bool s_used_prepared;  /* current request dialed a prepared ticket */
static bool s_retry_cold;     /* that ticket was refused: mint once more */

/* Always-warm fast route (dev5): while idle, paired and unmuted the box
 * keeps one prepared plain session and, when the server keeps unstarted
 * sockets open, its WebSocket already dialed ("pre-dial"). A tap then only
 * sends `start`. The pair is replaced every WARM_CYCLE_US, which also keeps
 * the API keep-alive connection from idling out, so at most ~1.2 mints and
 * DELETEs per minute. A server that closes an unstarted socket (today: after
 * 5 s, start_required) turns pre-dial off for PREDIAL_BLOCK_US; the prepared
 * ticket alone still saves the mint. */
#define WARM_CYCLE_US ((int64_t)50 * 1000 * 1000)
#define PREDIAL_EARLY_CLOSE_US ((int64_t)20 * 1000 * 1000)
#define PREDIAL_BLOCK_US ((int64_t)30 * 60 * 1000 * 1000)
/* The account token is refreshed in the background before a tap needs it:
 * cloud_auth refreshes once less than 10 min are left. */
#define AUTH_CHECK_US ((int64_t)30 * 1000 * 1000)
static int64_t s_prepared_minted_us;
static bool s_predial;              /* s_client is the prepared session's idle socket */
static volatile bool s_ws_dropped;  /* the current socket ended (events only) */
static volatile bool s_start_sent;  /* start went out on the current socket */
static volatile int64_t s_ws_dial_us;
static volatile int32_t s_ws_dial_ms = -1;  /* TCP+TLS+upgrade of the current socket */
static int64_t s_predial_connected_us;
static int64_t s_predial_blocked_until_us;
static int64_t s_auth_check_us;
static int32_t s_last_api_tls_ms = -1;      /* last API exchange: fresh handshake ms, -1 reused */
static int32_t s_t_auth_ms = -1, s_t_api_tls_ms = -1, s_t_ws_tls_ms = -1;
static volatile bool s_tls_api, s_tls_ws;   /* a handshake is running (UI throttles) */
static bool s_last_paired;
static bool s_paired_seen;

/* ── Errors (s_lock) and per-tap timing ──────────────────────────────────── */
static kbx_voice_error_t s_error;
static int64_t s_error_us;
static uint32_t s_error_revision;
static kbx_voice_error_t s_attempt_err; /* net_ws_task only */
static volatile int s_ws_err_status;    /* WS upgrade HTTP status, 0 = none */
static volatile bool s_ws_was_connected;
static volatile bool s_session_end_seen;
static volatile bool s_turn_listened;
static int64_t s_t_tap, s_t_history, s_t_session, s_t_ws, s_t_listen, s_t_uplink, s_t_reply;
static uint32_t s_t_buffered_ms;
static const char *s_t_path = "cold";
static bool s_t_logged;
static bool s_t_failed;

/* Tap captures real 20 ms frames immediately, independently of TLS. Only the
 * lifecycle task drains them after the server's listening acknowledgement.
 * Server VAD commits turns; tap-off while listening sends audio_end. */
#define TALK_MAX_US ((int64_t)120 * 1000 * 1000)
#define TALK_STARTUP_US ((int64_t)30 * 1000 * 1000)
#define TALK_MODE_ACK_US ((int64_t)15 * 1000 * 1000)
#define TALK_REPLY_US ((int64_t)30 * 1000 * 1000)
/* First reply of a Companion turn: its first content measures 13 to 17 s. */
#define SWITCH_REPLY_US ((int64_t)60 * 1000 * 1000)
#define TALK_NO_INPUT_US ((int64_t)15 * 1000 * 1000)

/* Outbound/lifetime gate. Outside the WS callback the order is client ->
 * state -> PCM. Release state BEFORE any websocket send/close/destroy:
 * WS dispatch may own its internal mutex while waiting for state. DATA
 * callbacks never take this gate. External revocations take it before state
 * so no validated send remains in flight after revocation returns. */
static SemaphoreHandle_t s_client_lock = NULL;
static TaskHandle_t s_talk_task = NULL;
static volatile bool s_talk_active = false;
static volatile uint32_t s_talk_frames = 0;   /* sent audio frames */
static volatile uint32_t s_talk_faults = 0;   /* capture/send faults */
static volatile uint32_t s_talk_starts = 0;   /* started talk phases */
/* Agent-on arms the device; only this explicit, bounded request permits
 * provider work. There is deliberately no automatic retry after teardown. */
static volatile bool s_requested;
/* Ephemeral consent for follow-up capture; ordinary Talk stays one-shot. */
static volatile bool s_conversation;
/* Front door route_switch (SpeechKit "Device front door"): a plain session
 * that opted in with front_door routes ["agent"] is told to continue on the
 * Companion route. The receipt only sets s_switch_pending; the lifecycle task
 * closes the session, mints a Companion session for THIS request
 * (s_turn_agent; the saved route is never changed), replays the utterance as
 * the first text frame and plays the answer. The next tap or wake word starts
 * on the saved route again because cancel_request clears the override. */
static volatile bool s_switch_pending; /* receipt accepted, old session still open */
static volatile bool s_switch_text_pending; /* utterance still to be sent on the new session */
static volatile bool s_turn_agent;   /* this request runs on the Companion route */
static volatile bool s_switch_active; /* display: the Companion holds this turn */
static EXT_RAM_BSS_ATTR kbx_voice_route_switch_t s_switch;
static char s_last_switch_receipt[KBX_VOICE_CONTROL_ID_MAX + 1U];
/* UI consent queues control; only an authenticated server receipt enables
 * follow-up capture. No network call is made from the UI task. */
static bool s_mode_pending;
static bool s_mode_sent;
static bool s_mode_initial;
static int64_t s_mode_deadline_us;
static char s_mode_request_id[48];
static char s_last_mode_receipt[KBX_VOICE_CONTROL_ID_MAX + 1U];
static volatile bool s_media_ready;
static volatile bool s_capture_running;
static volatile bool s_capture_started;
static uint32_t s_capture_revision;
static volatile bool s_input_complete;
static volatile bool s_audio_end_sent;
static bool s_input_final_received;
static volatile bool s_reply_started;
static volatile bool s_reply_done;
static bool s_output_seen;
static int64_t s_request_deadline_us;
static int64_t s_reply_deadline_us;
static int64_t s_idle_since_us;
static int64_t s_no_input_deadline_us;
static bool s_input_heard;
/* Follow-up stages (dev15/dev16): after a reply that expects an answer, the
 * box takes another user turn on the same session and socket (no new
 * session, no re-dial), up to the owner's limit per tap or wake hit
 * (KBX assist followups, device_config assist.max_followups; 0-3, default
 * 3). Decided per reply: the server's expects_reply flag (SpeechKit, on
 * turn_end) when present, else a final output text ending in "?". Each
 * stage ends after 6 s without input. */
#define TALK_FOLLOWUP_NO_INPUT_US ((int64_t)6 * 1000 * 1000)
#define FOLLOWUPS_MAX 3U
#define FOLLOWUPS_DEFAULT 3U
static uint8_t s_max_followups = FOLLOWUPS_DEFAULT; /* setting, NVS kbx_assist */
/* Reply voice gender (dev21, KBX voice, device_config assist.voice), NVS
 * kbx_assist voice_v1: auto follows the selected wake phrase (male for
 * jarvis, hey_jarvis, mycroft, computer; female otherwise). */
static const char *const k_voice_ids[NET_WS_VOICE_COUNT] = { "auto", "female", "male" };
static volatile uint8_t s_voice_setting = NET_WS_VOICE_AUTO;
static uint8_t s_followups_left;    /* stages still allowed in this request */
static uint8_t s_stage;             /* 0 = the tap/wake turn, 1.. = follow-ups */
static bool s_followup_turn;        /* the current capture is a follow-up */
static int8_t s_expects_reply = -1; /* server signal this turn: -1 none, 0, 1 */
static char s_out_tail[8];          /* last bytes of the agent's output text */
/* The server ended the input turn or closed: no more uplink this turn. */
static bool s_uplink_stopped;
/* dev17: the socket stopped taking uplink audio since this time (0 = no).
 * Audio stays in the PSRAM FIFO meanwhile; the turn ends only when the
 * wait or the backlog exceeds ~3 s, or the socket really closed. */
static int64_t s_uplink_blocked_since_us;
#define UPLINK_BACKLOG_MAX_US ((int64_t)3 * 1000 * 1000)
#define UPLINK_BACKLOG_MAX_FRAMES 150U
static int s_close_code;            /* last close frame code, 0 = none */
/* Backstop only: writes go out when the socket reports room (dev15's
 * 300 ms per-frame limit failed on ordinary Wi-Fi gaps). */
#define UPLINK_SEND_TIMEOUT_MS 2500
static SemaphoreHandle_t s_pcm_lock;
static EXT_RAM_BSS_ATTR kbx_voice_buffer_t s_pcm;
static EXT_RAM_BSS_ATTR kbx_voice_text_t s_text;

static int64_t no_input_us(void)
{
    return s_followup_turn ? TALK_FOLLOWUP_NO_INPUT_US : TALK_NO_INPUT_US;
}

/* UTF-8 tail ends in "?", fullwidth U+FF1F or Arabic U+061F. */
static bool tail_is_question(const char *t)
{
    size_t n = strlen(t);
    if (n >= 1 && t[n - 1] == '?') return true;
    if (n >= 3 && (uint8_t)t[n - 3] == 0xEF && (uint8_t)t[n - 2] == 0xBC && (uint8_t)t[n - 1] == 0x9F) return true;
    if (n >= 2 && (uint8_t)t[n - 2] == 0xD8 && (uint8_t)t[n - 1] == 0x9F) return true;
    return false;
}

/* Caller holds s_lock. The one place that decides a follow-up: the server
 * flag replaces the heuristic as soon as SpeechKit sends it. */
static bool followup_wanted_locked(const char **why)
{
    if (!s_followups_left) return false;
    if (s_expects_reply >= 0) {
        *why = "server expects_reply";
        return s_expects_reply == 1;
    }
    *why = "reply ends with a question";
    return tail_is_question(s_out_tail);
}

/* Display caption (UI 2, R-3/R-4/R-5): the latest transcript of the current
 * turn. Shown on the device only, never logged or persisted. Providers send
 * either cumulative text or deltas; a frame that extends the current text
 * replaces it, anything else is appended. A done frame closes the user's
 * input (it replaces it), while the reply arrives as one done frame per
 * sentence and accumulates (it used to show only the
 * latest few words). Only the tail is kept; the display trims it further. */
#define CAPTION_MAX 600
static EXT_RAM_BSS_ATTR char s_cap_text[CAPTION_MAX];
static net_ws_caption_kind_t s_cap_kind;
static bool s_cap_final;
static uint32_t s_cap_seq;
static portMUX_TYPE s_cap_mux = portMUX_INITIALIZER_UNLOCKED;

static void caption_reset(void)
{
    portENTER_CRITICAL(&s_cap_mux);
    s_cap_text[0] = '\0';
    s_cap_kind = NET_WS_CAPTION_NONE;
    s_cap_final = false;
    s_cap_seq++;
    portEXIT_CRITICAL(&s_cap_mux);
}

/* Keep the UTF-8 tail of src that fits CAPTION_MAX - 1 bytes. */
static const char *caption_tail(const char *src, size_t len)
{
    if (len < CAPTION_MAX) return src;
    const char *p = src + len - (CAPTION_MAX - 1);
    while (((unsigned char)*p & 0xC0) == 0x80) p++;
    return p;
}

static void caption_feed(net_ws_caption_kind_t kind, const char *text, bool done)
{
    static EXT_RAM_BSS_ATTR char next[CAPTION_MAX * 2];
    size_t len = strlen(text);
    portENTER_CRITICAL(&s_cap_mux);
    bool fresh = s_cap_kind != kind || (kind == NET_WS_CAPTION_INPUT && s_cap_final);
    size_t cur = fresh ? 0 : strlen(s_cap_text);
    bool extends = cur && len >= cur && !strncmp(text, s_cap_text, cur);
    if (fresh || extends || !cur || (done && kind == NET_WS_CAPTION_INPUT)) {
        strlcpy(s_cap_text, caption_tail(text, len), sizeof(s_cap_text));
    } else {
        memcpy(next, s_cap_text, cur);
        /* sentence frames carry no separating space */
        if (cur < sizeof(next) - 2 && next[cur - 1] != ' ' && text[0] != ' ') next[cur++] = ' ';
        size_t n = len < sizeof(next) - cur - 1 ? len : sizeof(next) - cur - 1;
        memcpy(next + cur, text, n);
        next[cur + n] = '\0';
        strlcpy(s_cap_text, caption_tail(next, cur + n), sizeof(s_cap_text));
    }
    s_cap_kind = kind;
    s_cap_final = done;
    s_cap_seq++;
    portEXIT_CRITICAL(&s_cap_mux);
}

bool net_ws_caption(char *out, size_t capacity, net_ws_caption_kind_t *kind, bool *final,
                    uint32_t *seq)
{
    if (!out || !capacity) return false;
    portENTER_CRITICAL(&s_cap_mux);
    strlcpy(out, s_cap_text, capacity);
    if (kind) *kind = s_cap_kind;
    if (final) *final = s_cap_final;
    if (seq) *seq = s_cap_seq;
    bool has = s_cap_text[0] != '\0';
    portEXIT_CRITICAL(&s_cap_mux);
    return has;
}

/* Public control changes wait out a selected outbound frame, then mutate
 * state. Never acquire the outbound gate while already holding state. */
static void control_lock(void)
{
    if (s_client_lock) xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void control_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
    if (s_client_lock) xSemaphoreGive(s_client_lock);
}

static void new_request_id(char *out, size_t capacity)
{
    snprintf(out, capacity, "box-%08x%08x%08x%08x", (unsigned)esp_random(),
        (unsigned)esp_random(), (unsigned)esp_random(), (unsigned)esp_random());
}

/* Caller holds state. The lifecycle worker sends this once, after Start's
 * listening acknowledgement. An uncertain result never starts capture. */
static void queue_conversation_mode(bool initial)
{
    new_request_id(s_mode_request_id, sizeof(s_mode_request_id));
    s_mode_pending = true;
    s_mode_sent = false;
    s_mode_initial = initial;
    s_mode_deadline_us = esp_timer_get_time() + TALK_STARTUP_US;
    s_no_input_deadline_us = 0;
    box_status_set(KBX_HOST_WAKE);
}

static void clear_pcm(bool valid)
{
    xSemaphoreTake(s_pcm_lock, portMAX_DELAY);
    kbx_voice_buffer_reset(&s_pcm, valid);
    xSemaphoreGive(s_pcm_lock);
}

/* Listening earcons (dev18): one start cue when a capture turn begins (tap,
 * wake hit, follow-up or conversation turn, the latter after the reply has
 * played) and one end cue when that listening ends for any reason. Non-
 * blocking (audio_cue_play only notifies the cue task), so it is safe in the
 * WS callback and under s_lock. */
static bool s_cue_listening;

static void cue_listen_start(void)
{
    s_cue_listening = true;
    audio_cue_play(AUDIO_CUE_LISTEN_START);
}

static void cue_listen_end(void)
{
    if (!s_cue_listening) return;
    s_cue_listening = false;
    audio_cue_play(AUDIO_CUE_LISTEN_END);
}

/* Caller holds s_lock, including callback and logout fences. Capture never
 * takes s_lock, so a bounded TLS/start send cannot starve its 20 ms reader. */
static void cancel_request(void)
{
    s_requested = false;
    s_conversation = false;
    s_mode_pending = false;
    s_mode_sent = false;
    s_mode_initial = false;
    s_mode_deadline_us = 0;
    s_mode_request_id[0] = '\0';
    s_last_mode_receipt[0] = '\0';
    s_switch_pending = false;
    s_switch_text_pending = false;
    s_turn_agent = false;
    s_switch_active = false;
    s_last_switch_receipt[0] = '\0';
    memset(&s_switch, 0, sizeof(s_switch)); /* the replayed speech is never kept */
    s_talk_active = false;
    cue_listen_end();
    s_media_ready = false;
    ++s_revision;
    clear_pcm(false);
    kbx_voice_text_reset(&s_text);
    s_ws_closed = true;
}

/* Caller holds s_lock. The first cause of a request's failure wins; it is
 * shown on the display until the next tap. Never contains credentials. */
static void publish_error_locked(const kbx_voice_error_t *e)
{
    if (s_error.kind != KBX_VOICE_ERR_NONE && s_error_revision == s_revision) return;
    s_error = *e;
    s_error_us = esp_timer_get_time();
    s_error_revision = s_revision;
    if (s_t_tap) s_t_failed = true;
    ESP_LOGW(TAG, "voice error kind=%s http=%d code=%s reason=%s request_id=%s",
             kbx_voice_error_name(e->kind), e->http_status, e->code,
             e->reason[0] ? e->reason : "-", e->request_id[0] ? e->request_id : "-");
    box_status_set(KBX_HOST_ERROR);
}

static void publish_locked(kbx_voice_error_kind_t kind, int status, const char *code)
{
    kbx_voice_error_t e;
    kbx_voice_error_set(&e, kind, status, code);
    publish_error_locked(&e);
}

/* Caller holds s_lock. An in-band server error with its sentence: the server
 * localizes it from the start frame's locale (de, en, es; anything else gets
 * English), so it is kept for display only where the display speaks that
 * language too (hi renders English on the device). */
static void publish_server_error_locked(const char *code, const cJSON *message)
{
    kbx_voice_error_t e;
    kbx_voice_error_set(&e, KBX_VOICE_ERR_NONE, 0, code);
    switch (i18n_requested_locale()) {
    case I18N_LOCALE_EN:
    case I18N_LOCALE_DE:
    case I18N_LOCALE_ES:
    case I18N_LOCALE_HI:
        if (cJSON_IsString(message) && message->valuestring[0] &&
            strlen(message->valuestring) < sizeof(e.message) &&
            kbx_voice_utf8_valid(message->valuestring))
            memcpy(e.message, message->valuestring, strlen(message->valuestring) + 1U);
        break;
    default:
        break;
    }
    publish_error_locked(&e);
}

static int ms_since_tap(int64_t t)
{
    return (t && s_t_tap) ? (int)((t - s_t_tap) / 1000) : -1;
}

/* Caller holds s_lock. Exactly one line per tap, at first reply audio or at
 * the end of the request, whichever comes first. -1 = phase not reached or
 * skipped by the warm path. */
static void log_timing_locked(const char *result)
{
    if (s_t_logged || !s_t_tap) return;
    s_t_logged = true;
    /* auth = token load (a refresh is a handshake to the login host),
     * api_tls / ws_tls = TCP+TLS(+upgrade) ms of this tap's connections;
     * -1 = reused or not needed, ws_tls of a pre-dialed socket is its idle
     * dial time. */
    ESP_LOGI(TAG, "voice-timing stage=%u path=%s tap=0 history=%d session=%d ws_open=%d listening=%d "
             "uplink=%d reply=%d buffered_ms=%u auth=%d api_tls=%d ws_tls=%d result=%s",
             (unsigned)s_stage, s_t_path, ms_since_tap(s_t_history), ms_since_tap(s_t_session),
             ms_since_tap(s_t_ws), ms_since_tap(s_t_listen), ms_since_tap(s_t_uplink),
             ms_since_tap(s_t_reply), (unsigned)s_t_buffered_ms, (int)s_t_auth_ms,
             (int)s_t_api_tls_ms, (int)s_t_ws_tls_ms, result);
}

/* A proven no-effect rejection during an existing turn must not abort its
 * answer. An uncertain failure or an initial Conversation request still
 * cancels; neither case grants follow-up capture. Caller holds state. */
static void reject_pending_mode(const char *code)
{
    if (s_mode_initial) {
        publish_locked(KBX_VOICE_ERR_UNKNOWN, 0, code);
        cancel_request();
        return;
    }
    strlcpy(s_last_mode_receipt, s_mode_request_id, sizeof(s_last_mode_receipt));
    s_mode_pending = false;
    s_mode_sent = false;
    s_mode_deadline_us = 0;
    s_mode_request_id[0] = '\0';
    s_no_input_deadline_us = esp_timer_get_time() + no_input_us();
    box_status_set(s_reply_done ? KBX_HOST_DONE : s_reply_started ? KBX_HOST_SPEAKING :
        (s_input_final_received || s_audio_end_sent) ? KBX_HOST_THINKING :
        s_media_ready ? KBX_HOST_LISTENING : KBX_HOST_WAKE);
}

static uint8_t s_dl_odd;
static bool s_dl_has_odd;
static int64_t s_dl_t0_us;
static uint32_t s_dl_bytes;
static bool s_dl_rate_logged;

static void downlink_reset(void)
{
    s_dl_has_odd = false;
    s_dl_odd = 0;
    s_dl_t0_us = 0;
    s_dl_bytes = 0;
    s_dl_rate_logged = false;
}

/* Caller holds state and the previous capture worker has stopped. Reuse the
 * same provider session, but accept only fresh microphone blocks and reset
 * every input/reply fence before another turn. Playback must already be idle. */
static bool begin_capture_turn(void)
{
    clear_pcm(true);
    if (audio_capture_start_16k() != ESP_OK) {
        clear_pcm(false);
        return false;
    }
    s_talk_active = true;
    s_capture_running = true;
    cue_listen_start();
    s_capture_started = true;
    s_capture_revision = s_revision;
    s_input_complete = false;
    s_audio_end_sent = false;
    s_input_final_received = false;
    s_reply_started = false;
    s_reply_done = false;
    s_output_seen = false;
    s_idle_since_us = 0;
    s_reply_deadline_us = 0;
    s_input_heard = false;
    s_expects_reply = -1;
    s_out_tail[0] = 0;
    s_uplink_stopped = false;
    s_uplink_blocked_since_us = 0;
    int64_t now = esp_timer_get_time();
    s_no_input_deadline_us = s_media_ready ? now + no_input_us() : 0;
    s_request_deadline_us = now + (s_media_ready ? TALK_MAX_US : TALK_STARTUP_US);
    audio_play_agent_flush();
    audio_play_agent_resume();
    downlink_reset();
    /* The microphone records into the PSRAM FIFO from this instant; nothing
     * said now is lost, so the display may say Listening right away. */
    box_status_set(KBX_HOST_LISTENING);
    return true;
}

static void downlink_feed(const uint8_t *p, size_t n)
{
    if (!p || n == 0) {
        return;
    }
    if (s_dl_t0_us == 0) {
        s_dl_t0_us = esp_timer_get_time();
    }
    s_dl_bytes += (uint32_t)n;
    if (!s_dl_rate_logged && s_dl_bytes >= 16000) {
        int64_t dt = esp_timer_get_time() - s_dl_t0_us;
        if (dt > 200000) {
            uint32_t hz = (uint32_t)((uint64_t)s_dl_bytes * 500000ull / (uint64_t)dt);
            ESP_LOGI(TAG, "downlink rate ~%u Hz (%u B in %d ms) play_src=%u",
                     (unsigned)hz, (unsigned)s_dl_bytes, (int)(dt / 1000),
                     (unsigned)audio_play_agent_src_hz());
            s_dl_rate_logged = true;
        }
    }
    uint8_t pair[2];
    if (s_dl_has_odd) {
        pair[0] = s_dl_odd;
        pair[1] = p[0];
        (void)audio_play_agent_24k((const int16_t *)pair, 1);
        p++;
        n--;
        s_dl_has_odd = false;
    }
    if (n & 1U) {
        s_dl_odd = p[n - 1];
        s_dl_has_odd = true;
        n--;
    }
    if (n >= 2) {
        (void)audio_play_agent_24k((const int16_t *)p, n / 2);
    }
}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: int=%u largest=%u spiram=%u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static bool provisioned(void)
{
    bool ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool cloud = kbx_cloud_server(s_url);
    ok = s_url[0] != '\0' && (cloud || s_token[0] != '\0');
    xSemaphoreGive(s_lock);
    if (ok && cloud) {
        cloud_auth_status_t status;
        cloud_auth_get_status(&status);
        ok = status.paired;
    }
    return ok;
}

static void set_session_id(const char *v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_session_id, v ? v : "", sizeof(s_session_id));
    xSemaphoreGive(s_lock);
}

static void get_session_id(char *out, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_session_id, n);
    xSemaphoreGive(s_lock);
}

/* ── NVS ─────────────────────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_url);
    if (nvs_get_str(h, NET_NVS_KEY_URL, s_url, &n) != ESP_OK) {
        s_url[0] = '\0';
    }
    n = sizeof(s_token);
    if (nvs_get_str(h, NET_NVS_KEY_TOKEN, s_token, &n) != ESP_OK) {
        s_token[0] = '\0';
    }
    n = sizeof(s_provider);
    if (nvs_get_str(h, NET_NVS_KEY_PROVIDER, s_provider, &n) != ESP_OK) {
        s_provider[0] = '\0';
    }
    /* Standalone speaker: "agent on" survives reboot (u8, default 0 —
     * a freshly provisioned box stays off until KBX agent on). */
    uint8_t agent = 0;
    if (nvs_get_u8(h, NET_NVS_KEY_AGENT, &agent) == ESP_OK && agent) {
        s_agent_enabled = true;
    }
    /* Voice route (fast | agent) survives reboot; default fast. */
    uint8_t route = 0;
    if (nvs_get_u8(h, NET_NVS_KEY_ROUTE, &route) == ESP_OK) s_route_agent = route != 0;
    nvs_close(h);
}

/* Write ONE key to NVS (empty value = delete). nvs_set_str copies the value
 * immediately -> call under s_lock is ok. Blocking flash I/O, but only on
 * the rare provisioning action (same pattern as net_wifi_set_credentials). */
static void nvs_store(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (value && value[0]) {
        nvs_set_str(h, key, value);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ── HTTP: keep-alive API client, history, mint, delete (net_ws_task only) ── */

static bool request_allowed(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool allowed = s_requested && s_agent_enabled && !s_suspended &&
        s_session_revision == s_revision && esp_timer_get_time() < s_request_deadline_us;
    xSemaphoreGive(s_lock);
    return allowed;
}

/* Background preparation stays valid only for the identity it started with. */
static bool prepare_allowed(uint32_t epoch)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool allowed = s_agent_enabled && !s_suspended && s_prov_epoch == epoch;
    xSemaphoreGive(s_lock);
    return allowed;
}

static void attempt_error(kbx_voice_error_kind_t kind, int status, const char *code)
{
    if (s_attempt_err.kind == KBX_VOICE_ERR_NONE) kbx_voice_error_set(&s_attempt_err, kind, status, code);
}

static void api_drop(void)
{
    if (s_api) {
        esp_http_client_close(s_api);
        esp_http_client_cleanup(s_api);
        s_api = NULL;
    }
    s_api_warm = false;
    s_api_open = false;
    s_api_host[0] = '\0';
}

static bool url_host(const char *url, char *out, size_t capacity)
{
    const char *start = strstr(url, "://");
    if (!start) return false;
    start += 3;
    size_t n = strcspn(start, "/:?#");
    if (n == 0 || n >= capacity) return false;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

/* One client per API host. A host change drops the old TLS connection. */
static esp_http_client_handle_t api_client(const char *url)
{
    char host[sizeof(s_api_host)];
    if (!url_host(url, host, sizeof(host))) return NULL;
    if (s_api && strcmp(host, s_api_host) != 0) api_drop();
    if (!s_api) {
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .buffer_size_tx = NET_WS_TOKEN_MAX + 512,
            .disable_auto_redirect = true,
            /* TCP keep-alive holds NAT state and detects a dead peer, so a
             * reused connection fails fast instead of at the read timeout. */
            .keep_alive_enable = true,
            .keep_alive_idle = 20,
            .keep_alive_interval = 5,
            .keep_alive_count = 3,
        };
        s_api = esp_http_client_init(&cfg);
        if (!s_api) return NULL;
        strlcpy(s_api_host, host, sizeof(s_api_host));
        s_api_warm = false;
        s_api_open = true;
        return s_api;
    }
    return esp_http_client_set_url(s_api, url) == ESP_OK ? s_api : NULL;
}

/* One HTTP exchange on the persistent client. Any status is returned with its
 * body in s_http_resp; a transport failure records the attempt error. Only a
 * reused connection that died before any response byte is retried once on a
 * fresh connection: the server closed it, so it never saw this request. */
static esp_err_t api_request(esp_http_client_method_t method, const char *url,
    const char *body, const char *key, const char *auth, int *status_out)
{
    *status_out = 0;
    s_http_resp[0] = '\0';
    int length = body ? (int)strlen(body) : 0;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; ++attempt) {
        esp_http_client_handle_t c = api_client(url);
        if (!c) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        bool reused = s_api_warm;
        bool answered = false;
        s_api_warm = false;
        esp_http_client_set_method(c, method);
        esp_http_client_set_header(c, "Authorization", auth);
        if (body) esp_http_client_set_header(c, "Content-Type", "application/json");
        else esp_http_client_delete_header(c, "Content-Type");
        if (key) esp_http_client_set_header(c, "Idempotency-Key", key);
        else esp_http_client_delete_header(c, "Idempotency-Key");
        int64_t t_open = esp_timer_get_time();
        if (!reused) s_tls_api = true;
        err = esp_http_client_open(c, length);
        s_tls_api = false;
        if (!reused) {
            s_last_api_tls_ms = (int32_t)((esp_timer_get_time() - t_open) / 1000);
            ESP_LOGI(TAG, "api tls handshake %d ms (%s)", (int)s_last_api_tls_ms,
                     err == ESP_OK ? "ok" : esp_err_to_name(err));
        } else {
            s_last_api_tls_ms = -1;
        }
        if (err == ESP_OK && length > 0 && esp_http_client_write(c, body, length) != length)
            err = ESP_FAIL;
        if (err == ESP_OK) {
            int64_t clen = esp_http_client_fetch_headers(c);
            if (clen == -ESP_ERR_HTTP_EAGAIN) {
                err = ESP_ERR_TIMEOUT;
            } else if (clen < 0) {
                err = ESP_FAIL;
            } else {
                answered = true;
                if (clen >= (int64_t)sizeof(s_http_resp)) err = ESP_ERR_INVALID_SIZE;
            }
        }
        if (err == ESP_OK) {
            int n = esp_http_client_read_response(c, s_http_resp, sizeof(s_http_resp) - 1);
            if (n < 0 || !esp_http_client_is_complete_data_received(c)) {
                err = n == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
            } else {
                s_http_resp[n] = '\0';
                *status_out = esp_http_client_get_status_code(c);
            }
        }
        /* The header copy lives in the client; never keep a credential there. */
        esp_http_client_delete_header(c, "Authorization");
        if (err == ESP_OK && esp_http_client_is_persistent_connection(c)) {
            s_api_warm = true;
        } else {
            esp_http_client_close(c);
        }
        if (err == ESP_OK || !reused || answered || err == ESP_ERR_TIMEOUT) break;
        ESP_LOGI(TAG, "kept-alive API connection was closed; reconnecting once");
    }
    if (err != ESP_OK) {
        bool wifi = net_wifi_is_connected(NULL, 0);
        ESP_LOGW(TAG, "cloud request transport failed: %s (wifi=%s)", esp_err_to_name(err),
                 wifi ? "up" : "down");
        if (!wifi) attempt_error(KBX_VOICE_ERR_OFFLINE, 0, "wifi");
        else if (err == ESP_ERR_HTTP_CONNECT) attempt_error(KBX_VOICE_ERR_OFFLINE, 0, "connect");
        else if (err == ESP_ERR_TIMEOUT) attempt_error(KBX_VOICE_ERR_SERVICE, 0, "timeout");
        else if (err == ESP_ERR_INVALID_SIZE) attempt_error(KBX_VOICE_ERR_UNKNOWN, 0, "response_too_large");
        else if (err == ESP_ERR_NO_MEM) attempt_error(KBX_VOICE_ERR_UNKNOWN, 0, "no_memory");
        else attempt_error(KBX_VOICE_ERR_SERVICE, 0, "transport");
    }
    return err;
}

static esp_err_t api_post_json(const char *url, const char *body, const char *key)
{
    int status = 0;
    esp_err_t err = api_request(HTTP_METHOD_POST, url, body, key, s_http_auth, &status);
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) {
        kbx_voice_error_t e;
        kbx_voice_error_from_http(status, s_http_resp, &e);
        ESP_LOGW(TAG, "cloud request http %d code=%s reason=%s request_id=%s", status, e.code,
                 e.reason[0] ? e.reason : "-", e.request_id[0] ? e.request_id : "-");
        if (s_attempt_err.kind == KBX_VOICE_ERR_NONE) s_attempt_err = e;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Account bearer for the cloud, the provisioned service bearer otherwise. */
static esp_err_t load_auth(bool cloud)
{
    if (cloud && cloud_auth_access_token(s_request_token, sizeof(s_request_token)) != ESP_OK) {
        cloud_auth_status_t st;
        cloud_auth_get_status(&st);
        ESP_LOGW(TAG, "cloud credential unavailable (%s)", st.error_code[0] ? st.error_code : "-");
        if (!st.paired) attempt_error(KBX_VOICE_ERR_SIGNIN, 0, "signed_out");
        else if (!strcmp(st.error_code, "refresh_transport_failed"))
            attempt_error(net_wifi_is_connected(NULL, 0) ? KBX_VOICE_ERR_SERVICE : KBX_VOICE_ERR_OFFLINE,
                          0, "auth_connect");
        else if (!strcmp(st.error_code, "clock_not_ready"))
            attempt_error(KBX_VOICE_ERR_SERVICE, 0, "clock_not_ready");
        else if (!strcmp(st.error_code, "refresh_failed"))
            attempt_error(KBX_VOICE_ERR_SERVICE, 0, "auth_refresh_failed");
        else attempt_error(KBX_VOICE_ERR_SIGNIN, 0, st.error_code[0] ? st.error_code : "signin");
        memset(s_request_token, 0, sizeof(s_request_token));
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_http_auth, sizeof(s_http_auth), "Bearer %s", s_request_token);
    memset(s_request_token, 0, sizeof(s_request_token));
    return ESP_OK;
}

/* Creates one canonical Assist history into s_assist_session_id. The same
 * Idempotency-Key is reused until a history exists, so an ambiguous failure
 * never leaves an orphan behind. */
static esp_err_t create_history(void)
{
    static const char body[] = "{\"title\":\"New chat\",\"metadata\":{"
        "\"harnessId\":\"kombify-ai\",\"historyKind\":\"assist\","
        "\"speechkit\":{\"surface\":\"box\",\"voiceMode\":\"oneshot\"}}}";
    if (!s_warm_key[0]) new_request_id(s_warm_key, sizeof(s_warm_key));
    esp_err_t err = api_post_json(AI_SESSIONS_URL, body, s_warm_key);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(s_http_resp);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    bool valid = kbx_voice_control_id(id);
    if (valid) strlcpy(s_assist_session_id, id->valuestring, sizeof(s_assist_session_id));
    cJSON_Delete(root);
    if (!valid) {
        attempt_error(KBX_VOICE_ERR_UNKNOWN, 0, "bad_history_response");
        return ESP_FAIL;
    }
    s_warm_key[0] = '\0';
    return ESP_OK;
}

static void http_delete_session(const char *sid)
{
    if (!sid || !sid[0]) return;
    if (!kbx_session_url(s_session_base, sid, false, s_http_url, sizeof(s_http_url))) return;
    /* Cleanup uses the credential that minted this session, even after local
     * logout. It cannot renew or restore a forgotten account. */
    int status = 0;
    kbx_voice_error_t saved = s_attempt_err;
    if (api_request(HTTP_METHOD_DELETE, s_http_url, NULL, NULL, s_session_auth, &status) == ESP_OK) {
        ESP_LOGI(TAG, "session %s deleted (http %d)", sid, status);
    }
    s_attempt_err = saved; /* cleanup never replaces the request's cause */
}

/* Mint one voice session (history + ticket) on the keep-alive client.
 * prepare=false serves the current tap; prepare=true runs in the idle loop
 * and must stay valid for epoch. Fills s_ws_url, s_ws_subproto, the session
 * id, s_session_base/auth and s_ticket_expires. */
static esp_err_t mint_session(bool prepare, uint32_t epoch)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool allowed = s_url[0] != '\0' && s_agent_enabled && !s_suspended && s_prov_epoch == epoch &&
        (prepare || (s_requested && esp_timer_get_time() < s_request_deadline_us));
    if (!allowed) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_session_base, s_url, sizeof(s_session_base));
    strlcpy(s_request_token, s_token, sizeof(s_request_token));
    if (!prepare) s_session_revision = s_revision;
    bool cloud = kbx_cloud_server(s_session_base);
    xSemaphoreGive(s_lock);
    if (prepare && !cloud) {
        memset(s_request_token, 0, sizeof(s_request_token));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!kbx_session_url(s_session_base, NULL, false, s_http_url, sizeof(s_http_url))) {
        memset(s_request_token, 0, sizeof(s_request_token));
        attempt_error(KBX_VOICE_ERR_UNKNOWN, 0, "server_url");
        return ESP_ERR_INVALID_ARG;
    }
    int64_t t_auth = esp_timer_get_time();
    esp_err_t err = load_auth(cloud);
    if (!prepare) s_t_auth_ms = (int32_t)((esp_timer_get_time() - t_auth) / 1000);
    if (err != ESP_OK) return err;
#define MINT_STILL_ALLOWED() (prepare ? prepare_allowed(epoch) : request_allowed())
    s_assist_session_id[0] = '\0';
#if CONFIG_KBX_DEV_ENDPOINTS
    const bool bench_agent = !cloud && s_turn_agent; /* PC mock: nothing to create a history on */
#else
    const bool bench_agent = false;
#endif
    bool agent_route = (cloud && (s_route_agent || s_turn_agent)) || bench_agent;
    if (agent_route) {
        if (bench_agent) {
            /* no Assist history on a self-hosted server */
        } else if (s_warm_history[0] && s_warm_history_epoch == epoch) {
            strlcpy(s_assist_session_id, s_warm_history, sizeof(s_assist_session_id));
        } else {
            if (!MINT_STILL_ALLOWED()) return ESP_ERR_INVALID_STATE;
            err = create_history();
            if (err != ESP_OK) return err;
            if (!prepare) s_t_history = esp_timer_get_time();
            /* Keep it for a retry until a session actually binds it, so a
             * failed mint never leaves an empty chat behind. */
            strlcpy(s_warm_history, s_assist_session_id, sizeof(s_warm_history));
            s_warm_history_epoch = epoch;
        }
        snprintf(s_http_body, sizeof(s_http_body), "{\"provider\":\"kombify-agent\","
            "\"target_agent_id\":\"workbench-orchestrator\",\"ai_session_id\":\"%s\","
            "\"harness_id\":\"kombify-ai\",\"composer_surface\":\"box\"}", s_assist_session_id);
    } else {
        /* The fast route names its surface too: on kombify Cloud the Gateway
         * then delegates the server-side tool credential to SpeechKit for this
         * session (consent and entitlements permitting). The Box never sees it. */
        strlcpy(s_http_body, cloud ? "{\"composer_surface\":\"box\"}" : "{}", sizeof(s_http_body));
    }
    if (!MINT_STILL_ALLOWED()) return ESP_ERR_INVALID_STATE;
    log_heap("before session-create");
    if (!kbx_session_url(s_session_base, NULL, false, s_http_url, sizeof(s_http_url))) return ESP_ERR_INVALID_ARG;
    err = api_post_json(s_http_url, s_http_body, NULL);
    if (!prepare) s_t_api_tls_ms = s_last_api_tls_ms;
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(s_http_resp);
    if (!root) {
        ESP_LOGW(TAG, "session-create: json parse failed");
        attempt_error(KBX_VOICE_ERR_UNKNOWN, 0, "bad_session_response");
        return ESP_FAIL;
    }
    const cJSON *jws = cJSON_GetObjectItemCaseSensitive(root, "ws_url");
    const cJSON *jsub = cJSON_GetObjectItemCaseSensitive(root, "ws_subprotocol");
    const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON *jexp = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    /* Optional display language pushed by the server (the account's locale).
     * Persisted only when it changes; absent = keep the current setting. */
    const cJSON *jloc = cJSON_GetObjectItemCaseSensitive(root, "locale");
    if (cJSON_IsString(jloc) && jloc->valuestring[0]) {
        (void)i18n_set_locale_tag(jloc->valuestring);
    }
    strlcpy(s_ticket_expires, cJSON_IsString(jexp) ? jexp->valuestring : "", sizeof(s_ticket_expires));
    esp_err_t out = ESP_FAIL;
    if (cJSON_IsString(jws) && cJSON_IsString(jsub) && cJSON_IsString(jsid)) {
        char sid[64];
        bool trunc = false;
        /* Only use a ticket at the authority that minted it; no response-
         * supplied URL may redirect device credentials or media elsewhere. */
        if (!kbx_session_url(s_session_base, jsid->valuestring, true,
                             s_ws_url, sizeof(s_ws_url))) {
            trunc = true;
        }
        if (strlcpy(s_ws_subproto, jsub->valuestring, sizeof(s_ws_subproto)) >= sizeof(s_ws_subproto)) {
            trunc = true;
        }
        if (strlcpy(sid, jsid->valuestring, sizeof(sid)) >= sizeof(sid)) {
            trunc = true;
        }
        if (trunc) {
            ESP_LOGW(TAG, "session-create: field too long (truncation) - aborted");
            set_session_id("");
        } else {
            set_session_id(sid);
            s_cloud_session = cloud;
            s_session_agent = agent_route;
            /* The history now belongs to this session. */
            s_warm_history[0] = '\0';
            strlcpy(s_session_auth, s_http_auth, sizeof(s_session_auth));
            out = ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "session-create: required fields missing");
    }
    cJSON_Delete(root);
    if (out != ESP_OK) attempt_error(KBX_VOICE_ERR_UNKNOWN, 0, "bad_session_response");
    return out;
#undef MINT_STILL_ALLOWED
}

/* Close the idle pre-dialed socket (net_ws task, no request bound). */
static void predial_close(void)
{
    if (s_client_lock) xSemaphoreTake(s_client_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_websocket_client_handle_t c = s_predial ? s_client : NULL;
    if (c) s_client = NULL;
    s_predial = false;
    if (c) s_ws_connected = false;
    xSemaphoreGive(s_lock);
    if (c) {
        esp_websocket_client_close(c, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(c);
    }
    if (s_client_lock) xSemaphoreGive(s_client_lock);
    s_tls_ws = false;
    s_predial_connected_us = 0;
}

/* Drop the prepared session: DELETE settles its reservation at zero. */
static void discard_prepared(const char *why)
{
    if (s_predial) predial_close();
    if (!s_prepared) return;
    char sid[64];
    get_session_id(sid, sizeof(sid));
    ESP_LOGI(TAG, "prepared session discarded (%s)", why);
    s_prepared = false;
    s_prepared_until_us = 0;
    if (net_wifi_is_connected(NULL, 0)) http_delete_session(sid);
    set_session_id("");
    memset(s_session_auth, 0, sizeof(s_session_auth));
    /* The unstarted session never used its history: keep it for the next
     * turn instead of creating another empty chat. */
    if (!s_warm_history[0] && s_prepared_history[0] && s_prepared_epoch == s_prov_epoch) {
        strlcpy(s_warm_history, s_prepared_history, sizeof(s_warm_history));
        s_warm_history_epoch = s_prepared_epoch;
    }
    s_prepared_history[0] = '\0';
}

static void warm_backoff(void)
{
    int64_t delay = WARM_RETRY_MIN_US << (s_warm_failures < 5 ? s_warm_failures : 5);
    if (delay > WARM_RETRY_MAX_US) delay = WARM_RETRY_MAX_US;
    ++s_warm_failures;
    s_warm_retry_us = esp_timer_get_time() + delay;
}

static void prefetch_history(uint32_t epoch)
{
    memset(&s_attempt_err, 0, sizeof(s_attempt_err));
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = load_auth(true);
    if (err == ESP_OK && prepare_allowed(epoch)) err = create_history();
    else if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    memset(s_http_auth, 0, sizeof(s_http_auth));
    if (err == ESP_OK && prepare_allowed(epoch)) {
        strlcpy(s_warm_history, s_assist_session_id, sizeof(s_warm_history));
        s_warm_history_epoch = epoch;
        s_warm_failures = 0;
        ESP_LOGI(TAG, "warm: next Assist history ready in %d ms",
                 (int)((esp_timer_get_time() - t0) / 1000));
    } else {
        warm_backoff();
        ESP_LOGW(TAG, "warm: history prefetch failed (%s); retry in %d s",
                 s_attempt_err.code[0] ? s_attempt_err.code : esp_err_to_name(err),
                 (int)((s_warm_retry_us - esp_timer_get_time()) / 1000000));
    }
}

static void prepare_session(uint32_t epoch)
{
    memset(&s_attempt_err, 0, sizeof(s_attempt_err));
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = mint_session(true, epoch);
    memset(s_http_auth, 0, sizeof(s_http_auth));
    if (err != ESP_OK) {
        set_session_id("");
        memset(s_session_auth, 0, sizeof(s_session_auth));
        ESP_LOGW(TAG, "warm: prepared session skipped (%s)",
                 s_attempt_err.code[0] ? s_attempt_err.code : esp_err_to_name(err));
        if (err != ESP_ERR_INVALID_STATE) warm_backoff();
        return;
    }
    s_warm_failures = 0;
    s_prepared_minted_us = t0;
    int64_t ttl = kbx_warm_ticket_ttl_s(s_ticket_expires, (int64_t)time(NULL));
    s_prepared_until_us = kbx_warm_usable_until_us(t0, ttl);
    s_prepared_epoch = epoch;
    strlcpy(s_prepared_history, s_assist_session_id, sizeof(s_prepared_history));
    s_prepared = true;
    if (!prepare_allowed(epoch) || !s_prepared_until_us) {
        discard_prepared(s_prepared_until_us ? "identity changed" : "ticket too short");
        return;
    }
    ESP_LOGI(TAG, "warm: prepared session ready in %d ms (ticket %d s, held <= %d s)",
             (int)((esp_timer_get_time() - t0) / 1000), (int)ttl,
             (int)((s_prepared_until_us - t0) / 1000000));
}

static void predial_start(void);

/* Background token refresh: a tap must never wait for the login host. */
static void auth_tick(int64_t now)
{
    if (now < s_auth_check_us) return;
    s_auth_check_us = now + AUTH_CHECK_US;
    int left = cloud_auth_access_valid_s();
    if (left > 660) return;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = cloud_auth_access_token(s_request_token, sizeof(s_request_token));
    memset(s_request_token, 0, sizeof(s_request_token));
    ESP_LOGI(TAG, "warm: account token %s in %d ms (had %d s left)",
             err == ESP_OK ? "refreshed" : "refresh failed",
             (int)((esp_timer_get_time() - t0) / 1000), left);
}

/* Idle-loop housekeeping: expire/discard, then prepare at most one item per
 * pass. A tap is accepted meanwhile; its request runs right after. */
static void warm_tick(bool wifi, bool provisioned_now, bool cloud)
{
    uint32_t epoch = s_prov_epoch;
    int64_t now = esp_timer_get_time();
    bool usable = s_agent_enabled && !s_suspended && wifi && provisioned_now && cloud &&
        !audio_is_muted();
    s_bg_busy = true;
    if (usable) auth_tick(now);
    bool predial_dead = s_predial && (s_ws_dropped ||
        (!s_ws_connected && now > s_connect_deadline_us));
    if (predial_dead) {
        /* An unstarted socket the server closed soon after the upgrade
         * means it does not hold them (start_required): stop pre-dialing
         * for a while; the prepared ticket path stays. */
        bool early = s_predial_connected_us &&
            now - s_predial_connected_us < PREDIAL_EARLY_CLOSE_US;
        if (early) {
            s_predial_blocked_until_us = now + PREDIAL_BLOCK_US;
            ESP_LOGW(TAG, "warm: server closed the idle socket after %d ms; pre-dial off for %d min",
                     (int)((now - s_predial_connected_us) / 1000),
                     (int)(PREDIAL_BLOCK_US / 60000000));
        } else if (!s_predial_connected_us) {
            warm_backoff();
            ESP_LOGW(TAG, "warm: pre-dial failed (ws status %d)", s_ws_err_status);
        }
        discard_prepared("pre-dialed socket closed");
    } else if (s_prepared && (!usable || s_warm_hold || s_prepared_epoch != epoch ||
               !kbx_warm_usable(s_prepared_until_us, now) ||
               (!s_route_agent && now - s_prepared_minted_us >= WARM_CYCLE_US))) {
        discard_prepared(s_prepared_epoch != epoch ? "identity changed" :
            !usable ? "unavailable" : s_warm_hold ? "settings sync" :
            kbx_warm_usable(s_prepared_until_us, now) ? "refresh cycle" : "ticket expiring");
    } else if (s_warm_history[0] && (s_warm_history_epoch != epoch || !provisioned_now || !cloud)) {
        s_warm_history[0] = '\0';
    } else if (s_suspended || s_warm_hold) {
        /* Free the TLS session before an OTA download or a settings sync. */
        if (s_api) api_drop();
    } else if (usable && !s_route_agent && !s_prepared && now >= s_warm_retry_us) {
        /* Fast route: always one prepared plain session, pre-dialed when
         * the server keeps unstarted sockets. */
        s_prepare_wanted_until_us = 0;
        prepare_session(epoch);
        if (s_prepared && esp_timer_get_time() >= s_predial_blocked_until_us) predial_start();
    } else if (usable && s_route_agent && !s_prepared && !s_warm_history[0] && now >= s_warm_retry_us) {
        /* A prepared session carries its own history: never two spares. */
        prefetch_history(epoch);
    } else if (usable && s_route_agent && s_prepare_wanted_until_us && !s_prepared && s_warm_history[0]) {
        bool wanted = now < s_prepare_wanted_until_us;
        s_prepare_wanted_until_us = 0;
        if (wanted) prepare_session(epoch);
    }
    s_bg_busy = false;
}

/* ── WebSocket ───────────────────────────────────────────────────────────── */

/* Called only under the callback's state fence. No transcript or provider
 * content is logged; phase metadata is shared by all server frame types. */
/* Tool activity of the current turn (Voice Agent with tools): the display
 * shows a working state while a server tool runs. Only the tool's name is
 * kept (ASCII, short); arguments and results never leave the frame. */
#define TOOL_SHOW_US (12LL * 1000 * 1000)
static char s_tool_name[40];
static volatile int64_t s_tool_until_us;
static portMUX_TYPE s_tool_mux = portMUX_INITIALIZER_UNLOCKED;

static void tool_activity(const char *name, bool done)
{
    taskENTER_CRITICAL(&s_tool_mux);
    if (done) {
        s_tool_until_us = 0;
    } else {
        size_t n = 0;
        for (; name && name[n] && n < sizeof(s_tool_name) - 1; ++n) {
            const char c = name[n];
            s_tool_name[n] = (c >= 32 && c < 127) ? c : '?';
        }
        s_tool_name[n] = '\0';
        s_tool_until_us = esp_timer_get_time() + TOOL_SHOW_US;
    }
    taskEXIT_CRITICAL(&s_tool_mux);
}

bool net_ws_tool_activity(char *name, size_t capacity)
{
    taskENTER_CRITICAL(&s_tool_mux);
    const bool active = s_tool_until_us && esp_timer_get_time() < s_tool_until_us;
    if (active && name && capacity) strlcpy(name, s_tool_name, capacity);
    taskEXIT_CRITICAL(&s_tool_mux);
    return active;
}

static void handle_ws_text(const char *buf, size_t length)
{
    cJSON *root = cJSON_ParseWithLengthOpts(buf, length + 1U, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        publish_locked(KBX_VOICE_ERR_UNKNOWN, 0, "bad_frame");
        cancel_request();
        return;
    }
    if (s_switch_pending) {
        /* The Companion takes this turn: nothing more of the old session counts. */
        cJSON_Delete(root);
        return;
    }
    unsigned phases = kbx_voice_phases(root);
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "type");
    {
        /* Every inbound control frame of a turn, without any text content
         * (transcripts are never logged): type, event, state, done, size. */
        const cJSON *jev = cJSON_GetObjectItemCaseSensitive(root, "event_type");
        const cJSON *jst = cJSON_GetObjectItemCaseSensitive(root, "state");
        const cJSON *jmeta = cJSON_GetObjectItemCaseSensitive(root, "provider_metadata");
        const cJSON *jpe = cJSON_GetObjectItemCaseSensitive(jmeta, "provider_event");
        const cJSON *jer = cJSON_GetObjectItemCaseSensitive(root, "expects_reply");
        ESP_LOGI(TAG, "rx %s%s%s%s%s%s%s%s%s len=%u",
                 cJSON_IsString(jt) ? jt->valuestring : "?",
                 cJSON_IsString(jev) ? " event=" : "", cJSON_IsString(jev) ? jev->valuestring : "",
                 cJSON_IsString(jst) ? " state=" : "", cJSON_IsString(jst) ? jst->valuestring : "",
                 cJSON_IsString(jpe) ? " provider=" : "", cJSON_IsString(jpe) ? jpe->valuestring : "",
                 cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "done")) ? " done" : "",
                 cJSON_IsBool(jer) ? (cJSON_IsTrue(jer) ? " expects_reply" : " no_reply") : "",
                 (unsigned)length);
        if (cJSON_IsBool(jer)) s_expects_reply = cJSON_IsTrue(jer) ? 1 : 0;
    }
    if (cJSON_IsString(jt)) {
        const char *type = jt->valuestring;
        {
            const cJSON *jev = cJSON_GetObjectItemCaseSensitive(root, "event_type");
            const char *ev = cJSON_IsString(jev) ? jev->valuestring : "";
            const cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
            if (!strcmp(type, "tool_call") || !strcmp(ev, "tool_call")) {
                tool_activity(cJSON_IsString(jname) ? jname->valuestring : "tool", false);
                ESP_LOGI(TAG, "tool call: %s", s_tool_name);
            } else if (!strcmp(ev, "tool_result_ack")) {
                tool_activity(NULL, true);
            } else if (!strcmp(type, "output_transcript") || !strcmp(type, "session_end")) {
                tool_activity(NULL, true);
            } else if (!strcmp(type, "sequence_step")) {
                const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "status");
                const cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "step_id");
                ESP_LOGI(TAG, "sequence step %s: %s", cJSON_IsString(jid) ? jid->valuestring : "?",
                         cJSON_IsString(js) ? js->valuestring : "?");
            }
        }
        if (strcmp(type, "state") == 0) {
            const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "state");
            if (cJSON_IsString(js) && !strcmp(js->valuestring, "listening") && !s_media_ready) {
                s_media_ready = true;
                s_turn_listened = true;
                s_request_deadline_us = esp_timer_get_time() + TALK_MAX_US;
                s_no_input_deadline_us = s_mode_pending ? 0 : esp_timer_get_time() + no_input_us();
                if (!s_t_listen) {
                    s_t_listen = esp_timer_get_time();
                    xSemaphoreTake(s_pcm_lock, portMAX_DELAY);
                    s_t_buffered_ms = (uint32_t)s_pcm.count * 20U;
                    xSemaphoreGive(s_pcm_lock);
                }
                /* Input already ended by a second tap: the buffered turn is
                 * now being delivered, so the next visible phase is thinking. */
                box_status_set(s_mode_pending ? KBX_HOST_WAKE :
                    (s_talk_active ? KBX_HOST_LISTENING : KBX_HOST_THINKING));
            }
        } else if (strcmp(type, "error") == 0) {
            const cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "code");
            const char *code = cJSON_IsString(jc) ? jc->valuestring : "server_error";
            ESP_LOGW(TAG, "server-error code=%s", code);
            bool no_effect = !strcmp(code, "conversation_control_busy") ||
                !strcmp(code, "conversation_control_unsupported");
            if (s_mode_pending && no_effect) {
                reject_pending_mode(code);
            } else {
                publish_server_error_locked(code, cJSON_GetObjectItemCaseSensitive(root, "message"));
                cancel_request();
            }
        }
    }
    if (s_requested) {
        kbx_voice_control_t receipt;
        if (FRONT_DOOR_SESSION() && kbx_voice_control(root, s_session_id, &receipt) &&
            strcmp(s_last_mode_receipt, receipt.request_id)) {
            bool pending_match = s_mode_pending && !strcmp(s_mode_request_id, receipt.request_id);
            strlcpy(s_last_mode_receipt, receipt.request_id, sizeof(s_last_mode_receipt));
            if (!receipt.accepted) {
                ESP_LOGW(TAG, "conversation control rejected");
                if (pending_match) reject_pending_mode("conversation_control_rejected");
            } else {
                /* A spoken tool may finish while a panel request is queued.
                 * Apply the server's actual mode without acknowledging that
                 * separate request or releasing its uplink fence. */
                s_conversation = receipt.mode == KBX_VOICE_MODE_CONVERSATION;
                if (pending_match) {
                    s_mode_pending = false;
                    s_mode_sent = false;
                    s_mode_initial = false;
                    s_mode_deadline_us = 0;
                    s_mode_request_id[0] = '\0';
                }
                s_no_input_deadline_us = s_mode_pending ? 0 : esp_timer_get_time() + no_input_us();
                if (!s_conversation) {
                    s_talk_active = false;
                    cue_listen_end();
                    clear_pcm(false);
                } else if (!s_mode_pending && s_talk_active && s_media_ready) {
                    box_status_set(KBX_HOST_LISTENING);
                }
                ESP_LOGI(TAG, "authenticated conversation mode=%s history=ready",
                    s_conversation ? "conversation" : "one-shot");
            }
        }
        if (FRONT_DOOR_SESSION() && !s_session_agent && !s_switch_text_pending &&
            kbx_voice_route_switch(root, s_session_id, &s_switch)) {
            if (!strcmp(s_last_switch_receipt, s_switch.request_id)) {
                memset(&s_switch, 0, sizeof(s_switch));
            } else {
                strlcpy(s_last_switch_receipt, s_switch.request_id, sizeof(s_last_switch_receipt));
                ESP_LOGI(TAG, "route switch to the Companion (%s)", s_switch.reason);
                s_switch_pending = true;
                s_switch_active = true;
                /* The fast session's reply is muted; the Companion answers. */
                s_conversation = false;
                s_mode_pending = false;
                s_mode_sent = false;
                s_mode_initial = false;
                s_mode_deadline_us = 0;
                s_mode_request_id[0] = '\0';
                s_no_input_deadline_us = 0;
                s_reply_deadline_us = 0;
                s_uplink_stopped = true;
                s_talk_active = false;
                cue_listen_end();
                clear_pcm(false);
                audio_play_agent_flush();
                downlink_reset();
                box_status_set(KBX_HOST_THINKING);
                cJSON_Delete(root);
                return;
            }
        }
        if (phases & KBX_VOICE_INPUT_HEARD) s_input_heard = true;
        if (phases & KBX_VOICE_OUTPUT_SEEN) s_output_seen = true;
        {
            const cJSON *jt2 = cJSON_GetObjectItemCaseSensitive(root, "type");
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(jt2) && cJSON_IsString(text) && text->valuestring[0]) {
                bool done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "done"));
                if (!strcmp(jt2->valuestring, "input_transcript")) {
                    caption_feed(NET_WS_CAPTION_INPUT, text->valuestring, done);
                } else if (!strcmp(jt2->valuestring, "output_transcript")) {
                    caption_feed(NET_WS_CAPTION_OUTPUT, text->valuestring, done);
                }
                const cJSON *jev2 = cJSON_GetObjectItemCaseSensitive(root, "event_type");
                if (!strcmp(jt2->valuestring, "output_transcript") ||
                    !strcmp(jt2->valuestring, "output_text") ||
                    (cJSON_IsString(jev2) && !strcmp(jev2->valuestring, "output_text"))) {
                    /* Keep the tail of the agent's text (cumulative or delta,
                     * the last fragment ends the text either way). */
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
        if (phases & KBX_VOICE_INTERRUPTED) {
            downlink_reset();
            audio_play_agent_flush();
        }
        if (phases & KBX_VOICE_INPUT_FINAL) {
            s_input_final_received = true;
            s_talk_active = false;
            cue_listen_end();
            clear_pcm(false);
        }
        if ((phases & KBX_VOICE_INPUT_FINAL) && !s_reply_started && !s_reply_done) {
            s_reply_deadline_us = esp_timer_get_time() + TALK_REPLY_US;
            audio_play_agent_resume();
            box_status_set(KBX_HOST_THINKING);
        }
        if (kbx_voice_turn_ended(phases, s_output_seen) && !s_reply_done) {
            s_reply_done = true;
            s_talk_active = false;
            cue_listen_end();
            clear_pcm(false);
            audio_play_agent_complete();
            ESP_LOGI(TAG, "reply_complete pcm_bytes=%u src_hz=%u",
                     (unsigned)s_dl_bytes, (unsigned)audio_play_agent_src_hz());
            if (audio_play_agent_idle()) box_status_set(KBX_HOST_DONE);
        }
        if (phases & KBX_VOICE_SESSION_END) {
            s_session_end_seen = true;
            s_ws_closed = true;
        }
        /* The server ended this input turn (after input was heard) or the
         * session: stop sending. A late turn_end of the previous reply
         * cannot stop a fresh capture that has heard nothing yet. */
        bool input_turn_over = (phases & KBX_VOICE_TURN_END) &&
            (s_input_heard || s_input_final_received);
        if ((input_turn_over || (phases & KBX_VOICE_SESSION_END)) && !s_uplink_stopped) {
            s_uplink_stopped = true;
            if (s_talk_active) {
                s_talk_active = false;
                cue_listen_end();
                clear_pcm(false);
            }
            ESP_LOGI(TAG, "uplink stopped (%s)",
                     (phases & KBX_VOICE_SESSION_END) ? "session_end" : "turn_end");
        }
    }
    cJSON_Delete(root);
}

/* BCP-47 speech locale for the start frame. The server defaults to en, so
 * German speech was transcribed as English. Uses the requested UI language
 * (hi speaks Hindi even though the display falls back to English). */
static const char *speech_locale(void)
{
    switch (i18n_requested_locale()) {
    case I18N_LOCALE_DE: return "de-DE";
    case I18N_LOCALE_ES: return "es-ES";
    case I18N_LOCALE_ZH_HANS: return "zh-CN";
    case I18N_LOCALE_AR: return "ar-SA";
    case I18N_LOCALE_HI: return "hi-IN";
    case I18N_LOCALE_EN:
    default: return "en-US";
    }
}

/* Gender of the reply voice: the setting, or for auto the selected phrase
 * (a wake hit and a tap both use the phrase selected now). */
static bool speech_voice_male(void)
{
    switch (s_voice_setting) {
    case NET_WS_VOICE_MALE: return true;
    case NET_WS_VOICE_FEMALE: return false;
    default: return wakeword_phrase_male_voice(wakeword_get_phrase());
    }
}

/* Native reply voice for the UI language. The prod persona pins an English
 * Aura-2 voice that overrides the session locale, so German answers had an
 * English accent. Only languages with an Aura-2 voice in the SpeechKit TTS
 * catalog (profiles_tts.go) send one; the others keep the server default. */
static const char *speech_voice(void)
{
    const bool male = speech_voice_male();
    switch (i18n_requested_locale()) {
    case I18N_LOCALE_DE: return male ? "aura-2-julius-de" : "aura-2-viktoria-de";
    case I18N_LOCALE_EN: return male ? "aura-2-apollo-en" : "aura-2-thalia-en";
    default: return NULL;
    }
}

/* Caller holds s_lock. Server VAD commits turns (toggle-talk: box listens
 * until the user taps again); protocol.go StartFrame.ActivityDetection.
 * Provider only when explicitly set (empty = server default). */
static int build_start_locked(char *start_frame, size_t capacity)
{
    char prov[NET_WS_PROVIDER_MAX];
    strlcpy(prov, s_provider, sizeof(prov));
    const char *loc = speech_locale();
    const char *voice = speech_voice();
    char lang[96];
    if (voice) snprintf(lang, sizeof(lang), "\"locale\":\"%s\",\"voice\":\"%s\"", loc, voice);
    else snprintf(lang, sizeof(lang), "\"locale\":\"%s\"", loc);
    ESP_LOGI(TAG, "start: locale=%s voice=%s (assist.voice=%s, phrase=%s)", loc,
             voice ? voice : "server-default", k_voice_ids[s_voice_setting % NET_WS_VOICE_COUNT],
             wakeword_phrase_id(wakeword_get_phrase()));
    if (s_session_agent) { /* only ever set for a Cloud session (bench mock aside) */
        return snprintf(start_frame, capacity,
            "{\"type\":\"start\",\"media_provider\":\"deepgram\",\"media_transport\":\"websocket\",%s,"
            "\"conversation_control\":{\"version\":1,\"mode\":\"oneshot\"},"
            "\"activity_detection\":{\"automatic\":true}}", lang);
    }
    /* Front door: a plain session asks the server to recognise spoken mode
     * commands and Companion requests in the final transcript and to answer
     * with conversation_mode and route_switch receipts. Older servers ignore
     * the field; the receipts are applied only for this session. */
    const char *front_door = FRONT_DOOR_SESSION() ?
        ",\"front_door\":{\"version\":1,\"routes\":[\"agent\"]}" : "";
    if (prov[0]) {
        return snprintf(start_frame, capacity,
            "{\"type\":\"start\",\"media_transport\":\"websocket\",\"provider\":\"%s\","
            "%s,\"activity_detection\":{\"automatic\":true}%s}", prov, lang, front_door);
    }
    return snprintf(start_frame, capacity,
        "{\"type\":\"start\",\"media_transport\":\"websocket\",%s,"
        "\"activity_detection\":{\"automatic\":true}%s}", lang, front_door);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        int64_t now_us = esp_timer_get_time();
        s_tls_ws = false;
        s_ws_dial_ms = (int32_t)((now_us - s_ws_dial_us) / 1000);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ws_was_connected = true;
        if (s_predial) {
            /* Idle pre-dial: hold the socket; a tap sends start. */
            s_ws_connected = true;
            s_predial_connected_us = now_us;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "warm: ws pre-dialed in %d ms (tcp+tls+upgrade), idle until a tap",
                     (int)s_ws_dial_ms);
            break;
        }
        s_t_ws_tls_ms = s_ws_dial_ms;
        bool allowed = s_requested && s_agent_enabled && !s_suspended &&
            s_session_revision == s_revision && esp_timer_get_time() < s_request_deadline_us;
        if (!allowed) {
            s_ws_closed = true;
            xSemaphoreGive(s_lock);
            break;
        }
        downlink_reset();
        uint32_t freeh = esp_get_free_heap_size();
        ESP_LOGI(TAG, "ws connected in %d ms (heap free=%u, delta since open=%d) - sending start",
                 (int)s_ws_dial_ms, (unsigned)freeh, (int)((int64_t)s_heap_at_open - (int64_t)freeh));
        char start_frame[384];
        build_start_locked(start_frame, sizeof(start_frame));
        int sent = s_client ? esp_websocket_client_send_text(s_client, start_frame,
            (int)strlen(start_frame), pdMS_TO_TICKS(2000)) : -1;
        s_ws_connected = sent == (int)strlen(start_frame);
        s_start_sent = s_ws_connected;
        if (!s_t_ws) s_t_ws = esp_timer_get_time();
        if (!s_ws_connected) {
            publish_locked(KBX_VOICE_ERR_SERVICE, 0, "start_send");
            s_ws_closed = true;
        }
        /* Local capture keeps showing Listening; nothing else to change. */
        xSemaphoreGive(s_lock);
        break;
    }
    case WEBSOCKET_EVENT_DATA: {
        if (!ev) {
            break;
        }
        if (ev->op_code == 0x08) {
            /* Server close frame: log code and reason, end the uplink. */
            int code = 0;
            char reason[64] = "";
            if (ev->data_ptr && ev->data_len >= 2) {
                code = ((uint8_t)ev->data_ptr[0] << 8) | (uint8_t)ev->data_ptr[1];
                int n = ev->data_len - 2;
                if (n > (int)sizeof(reason) - 1) n = (int)sizeof(reason) - 1;
                for (int i = 0; i < n; i++) {
                    char c = ev->data_ptr[2 + i];
                    reason[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
                }
                reason[n > 0 ? n : 0] = 0;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            ESP_LOGI(TAG, "rx close code=%d reason=%s%s", code, reason[0] ? reason : "-",
                     s_predial ? " (pre-dialed socket)" : "");
            s_close_code = code ? code : 1005;
            if (!s_predial && s_requested) {
                s_uplink_stopped = true;
                s_talk_active = false;
                cue_listen_end();
                if (code == 1000) s_session_end_seen = true;
                s_ws_closed = true;
            }
            xSemaphoreGive(s_lock);
            break;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_predial) {
            /* Nothing is expected before start; a start_required notice
             * means the server is about to drop the idle socket. */
            if (ev->op_code == 0x01 && ev->data_ptr && ev->data_len > 0 &&
                ev->data_len < 512) {
                char tmp[512];
                memcpy(tmp, ev->data_ptr, (size_t)ev->data_len);
                tmp[ev->data_len] = '\0';
                if (strstr(tmp, "start_required")) {
                    s_ws_dropped = true;
                    ESP_LOGW(TAG, "warm: server requires start on the idle socket");
                }
            }
            xSemaphoreGive(s_lock);
            break;
        }
        if (!s_requested || !s_agent_enabled || s_suspended ||
            s_session_revision != s_revision || audio_is_muted() ||
            esp_timer_get_time() >= s_request_deadline_us) {
            xSemaphoreGive(s_lock);
            break;
        }
        if (ev->op_code == 0x01 || ev->op_code == 0x00) {
            kbx_voice_text_result_t result = KBX_VOICE_TEXT_INVALID;
            if (ev->data_len >= 0 && ev->payload_len >= 0 && ev->payload_offset >= 0) {
                result = kbx_voice_text_feed(&s_text, (uint8_t)ev->op_code,
                    ev->fin, (size_t)ev->payload_len, (size_t)ev->payload_offset,
                    ev->data_ptr, (size_t)ev->data_len);
            }
            if (result == KBX_VOICE_TEXT_COMPLETE) {
                handle_ws_text(s_text.bytes, s_text.used);
                kbx_voice_text_reset(&s_text);
            } else if (result == KBX_VOICE_TEXT_INVALID) {
                ESP_LOGW(TAG, "invalid or oversized voice control message");
                publish_locked(KBX_VOICE_ERR_UNKNOWN, 0, "bad_frame");
                cancel_request();
            }
        }
        else if (ev->op_code == 0x02 && ev->data_ptr && ev->data_len > 0) {
            if (s_switch_pending) {
                xSemaphoreGive(s_lock);
                break;
            }
            if (s_text.active) {
                publish_locked(KBX_VOICE_ERR_UNKNOWN, 0, "bad_frame");
                cancel_request();
                xSemaphoreGive(s_lock);
                break;
            }
            /* Stop the producer before invalidating its queue. Otherwise a
             * concurrent push could mistake half-duplex stop for overflow. */
            if (s_talk_active) {
                s_talk_active = false;
                cue_listen_end();
                ESP_LOGI(TAG, "downlink %d B - talk stop (half-duplex)", ev->data_len);
            }
            s_output_seen = true;
            if (!s_reply_started) {
                s_reply_started = true;
                clear_pcm(false);
            }
            s_reply_deadline_us = esp_timer_get_time() + TALK_REPLY_US;
            /* Raw S16 mono. Carry an odd leftover byte across WS fragments so
             * a split sample cannot misalign the rest of the turn. */
            audio_play_agent_resume();
            bool first = (box_status_get(NULL) != KBX_HOST_SPEAKING);
            downlink_feed((const uint8_t *)ev->data_ptr, (size_t)ev->data_len);
            if (first) {
                ESP_LOGI(TAG, "downlink start %d B src=%u",
                         ev->data_len, (unsigned)audio_play_agent_src_hz());
                box_status_set(KBX_HOST_SPEAKING);
            }
            if (!s_t_reply) {
                s_t_reply = esp_timer_get_time();
                log_timing_locked("ok");
            }
        }
        xSemaphoreGive(s_lock);
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        s_tls_ws = false;
        if (ev) {
            if (ev->error_handle.esp_ws_handshake_status_code > 0)
                s_ws_err_status = ev->error_handle.esp_ws_handshake_status_code;
            ESP_LOGW(TAG, "ws error: type=%d ws-http-status=%d tls_err=0x%x tls_stack=-0x%04x sock_errno=%d int=%u spiram=%u",
                     (int)ev->error_handle.error_type,
                     ev->error_handle.esp_ws_handshake_status_code,
                     ev->error_handle.esp_tls_last_esp_err,
                     -ev->error_handle.esp_tls_stack_err,
                     ev->error_handle.esp_transport_sock_errno,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGW(TAG, "ws error (no error data)");
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ws disconnected");
        s_tls_ws = false;
        s_ws_dropped = true;
        s_ws_connected = false;
        s_ws_closed = true;
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "ws closed");
        s_tls_ws = false;
        s_ws_dropped = true;
        s_ws_connected = false;
        s_ws_closed = true;
        break;
    default:
        break;
    }
}

/* Bind the current tap to a session: the prepared ticket when it is still
 * valid for this identity (no HTTP at all), otherwise a fresh mint on the
 * kept-alive API connection (with the prefetched history when available). */
static esp_err_t acquire_session(void)
{
    uint32_t epoch = s_prov_epoch;
    s_used_prepared = false;
    if (s_prepared && s_predial && (s_ws_dropped || !s_client)) {
        /* The upgrade spent the ticket: a dead idle socket cannot be reused. */
        discard_prepared("pre-dialed socket gone");
    }
    if (s_prepared) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool same_server = !strcmp(s_session_base, s_url);
        bool usable = !s_retry_cold && same_server && s_prepared_epoch == epoch &&
            kbx_warm_usable(s_prepared_until_us, esp_timer_get_time()) &&
            s_requested && s_agent_enabled && !s_suspended &&
            esp_timer_get_time() < s_request_deadline_us;
        bool predialed = usable && s_predial;
        if (usable) {
            s_session_revision = s_revision;
            s_used_prepared = true;
            s_predial = false; /* the socket now belongs to this tap */
        }
        xSemaphoreGive(s_lock);
        if (usable) {
            s_prepared = false;
            s_prepared_until_us = 0;
            s_prepared_history[0] = '\0';
            s_t_path = predialed ? (s_ws_connected ? "predial" : "predial-connecting") : "warm";
            return ESP_OK;
        }
        discard_prepared("not usable for this tap");
    }
    s_t_path = s_retry_cold ? "warm-retry" :
        (s_warm_history[0] && s_warm_history_epoch == epoch) ? "history" : "cold";
    esp_err_t err = mint_session(false, epoch);
    memset(s_http_auth, 0, sizeof(s_http_auth));
    if (err == ESP_OK) s_t_session = esp_timer_get_time();
    return err;
}

/* Create and start the WebSocket for the current session (s_ws_url). The
 * dial (TCP + TLS + upgrade) runs on the client's own task. */
static esp_err_t ws_dial(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = s_ws_url,
        .subprotocol = s_ws_subproto,
        /* The single-use ticket authenticates the upgrade. Never send an
         * account access token or the self-hosted service bearer as well. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 15000,
        .reconnect_timeout_ms = 10000,
        /* 15 s pings keep an idle pre-dialed socket (and NAT) alive. */
        .ping_interval_sec = 15,
        .task_stack = 24576, /* PSRAM: software-AES handshake needs more than 10 KB */
        .task_prio = 6,
        .buffer_size = 4096,
    };
    esp_websocket_client_handle_t c = esp_websocket_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "ws init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(c, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    s_ws_connected = false;
    s_ws_closed = false;
    s_ws_dropped = false;
    s_start_sent = false;
    s_ws_was_connected = false;
    s_ws_err_status = 0;
    s_ws_dial_ms = -1;
    s_ws_dial_us = esp_timer_get_time();
    s_connect_deadline_us = s_ws_dial_us + (int64_t)15 * 1000 * 1000;
    s_tls_ws = true;
    s_client = c;
    esp_err_t err = esp_websocket_client_start(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws start failed: %s", esp_err_to_name(err));
        s_client = NULL;
        s_tls_ws = false;
        esp_websocket_client_destroy(c);
    }
    return err;
}

/* net_ws task: a tap bound the already connected pre-dialed socket. A
 * failed send leaves s_start_sent false, so the lifecycle mints once more
 * inside the same tap without an error. */
static void send_start_now(void)
{
    char frame[384];
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool go = s_client && s_ws_connected && !s_ws_dropped && !s_start_sent && s_requested &&
        s_session_revision == s_revision && s_agent_enabled && !s_suspended;
    int len = go ? build_start_locked(frame, sizeof(frame)) : 0;
    esp_websocket_client_handle_t c = s_client;
    if (go) downlink_reset();
    xSemaphoreGive(s_lock);
    bool ok = go && esp_websocket_client_send_text(c, frame, len, pdMS_TO_TICKS(2000)) == len;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (go) {
        s_start_sent = ok;
        if (ok && !s_t_ws) s_t_ws = esp_timer_get_time();
        if (!ok) s_ws_closed = true;
        ESP_LOGI(TAG, "pre-dialed socket bound - start %s", ok ? "sent" : "failed");
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_client_lock);
}

/* net_ws task, idle: dial the prepared session's socket ahead of a tap. */
static void predial_start(void)
{
    if (s_client || !s_prepared) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_predial = true;
    s_predial_connected_us = 0;
    xSemaphoreGive(s_lock);
    if (ws_dial() != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_predial = false;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "warm: pre-dial could not start; ticket kept");
    }
}

/* Drop the outstanding session's server state after a failed bind. */
static void forget_session(void)
{
    char sid[64];
    get_session_id(sid, sizeof(sid));
    http_delete_session(sid);
    set_session_id("");
    memset(s_session_auth, 0, sizeof(s_session_auth));
}

static esp_err_t open_session(void)
{
    s_heap_at_open = esp_get_free_heap_size();
    memset(&s_attempt_err, 0, sizeof(s_attempt_err));
    s_session_end_seen = false;
    esp_err_t err = acquire_session();
    if (err != ESP_OK) {
        set_session_id("");
        memset(s_session_auth, 0, sizeof(s_session_auth));
        return err;
    }
    if (s_client) {
        /* Pre-dialed socket bound to this tap: no HTTP, no handshake. */
        s_ws_closed = s_ws_dropped;
        if (s_ws_connected) {
            s_t_ws_tls_ms = s_ws_dial_ms;
            send_start_now();
        } else {
            s_connect_deadline_us = esp_timer_get_time() + (int64_t)15 * 1000 * 1000;
        }
        return ESP_OK;
    }
    s_ws_err_status = 0;
    s_ws_was_connected = false;
    char sid[64];
    get_session_id(sid, sizeof(sid));
    if (!request_allowed()) {
        forget_session();
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    ESP_LOGI(TAG, "session=%s provider=%s -> ws-dial %s", sid,
             s_provider[0] ? s_provider : "(server-default)", s_ws_url);
    xSemaphoreGive(s_lock);
    log_heap("before ws-dial");

    err = ws_dial();
    if (err != ESP_OK) {
        attempt_error(KBX_VOICE_ERR_UNKNOWN, 0, err == ESP_ERR_NO_MEM ? "ws_init" : "ws_start");
        forget_session(); /* do not orphan the server session */
        return err;
    }
    return ESP_OK;
}

/* Transport-only teardown: a refused prepared ticket is replaced within the
 * same tap, so capture, PCM and the visible state stay untouched. */
static void close_transport(void)
{
    if (s_client_lock) xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client) {
        esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_client_lock) xSemaphoreGive(s_client_lock);
    s_ws_connected = false;
    forget_session();
}

static void close_session(void)
{
    s_talk_active = false; /* uplink task ends its phase on its own */
    cue_listen_end();
    s_media_ready = false;
    audio_play_agent_flush();
    if (s_client_lock) xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client) {
        /* An open socket always ends with stop and a close frame, and the
         * session DELETE (forget_session) only follows the close handshake:
         * the server must never see DELETE on a socket that is still open. */
        if (s_ws_connected) {
            static const char *STOP = "{\"type\":\"stop\"}";
            esp_websocket_client_send_text(s_client, STOP, (int)strlen(STOP), pdMS_TO_TICKS(1000));
        }
        esp_websocket_client_close(s_client, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_client_lock) xSemaphoreGive(s_client_lock);
    s_ws_connected = false;
    forget_session();
    memset(s_http_auth, 0, sizeof(s_http_auth));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    log_timing_locked(s_t_failed ? s_error.code : (s_t_reply ? "ok" : "ended"));
    /* A finished turn opens the follow-up window for one prepared ticket. */
    if (s_turn_listened && !s_t_failed)
        s_prepare_wanted_until_us = esp_timer_get_time() + PREPARE_WINDOW_US;
    s_turn_listened = false;
    /* Keep a fresh error visible; everything else returns to idle. */
    if (box_status_get(NULL) != KBX_HOST_ERROR) box_status_set(KBX_HOST_IDLE);
    xSemaphoreGive(s_lock);
}

/* The lifecycle task alone sends PCM. Capture runs independently even while
 * account renewal / session mint / TLS is blocking this task. */
static void send_pending_mode(void)
{
    char frame[192];
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool send = s_requested && s_cloud_session && s_media_ready && s_mode_pending &&
        !s_mode_sent && s_client && s_ws_connected && !s_ws_closed && !s_suspended &&
        s_agent_enabled && !audio_is_muted() && s_session_revision == s_revision;
    uint32_t revision = s_revision;
    esp_websocket_client_handle_t client = s_client;
    int length = 0;
    if (send) {
        length = snprintf(frame, sizeof(frame), "{\"type\":\"conversation_mode\",\"version\":1,"
            "\"mode\":\"conversation\",\"request_id\":\"%s\"}", s_mode_request_id);
        s_mode_sent = true;
        s_mode_deadline_us = esp_timer_get_time() + TALK_MODE_ACK_US;
    }
    xSemaphoreGive(s_lock);
    bool ok = !send || esp_websocket_client_send_text(client, frame, length,
        pdMS_TO_TICKS(500)) == length;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!ok && s_requested && s_mode_pending && revision == s_revision) {
        publish_locked(KBX_VOICE_ERR_SERVICE, 0, "mode_send");
        cancel_request();
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_client_lock);
}

/* Caller is the lifecycle task, a route_switch receipt was accepted. Leaves
 * the fast session (stop, close, DELETE: the account's conversation slot is
 * free again before the Companion session starts) and arms the next open for
 * the Companion route. The request, its capture state and the display stay. */
static void route_switch_to_companion(void)
{
    ESP_LOGI(TAG, "route switch: closing the fast session, opening the Companion route for this turn");
    close_session();
    s_active = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    s_switch_pending = false;
    s_switch_text_pending = true;
    s_turn_agent = true;
    s_session_end_seen = false;
    s_ws_closed = false;
    s_close_code = 0;
    s_uplink_stopped = false;
    /* The user's words are already in the utterance: no microphone upload on
     * the Companion session until its answer asks for one. */
    s_input_heard = true;
    s_input_final_received = true;
    s_audio_end_sent = true;
    s_reply_started = false;
    s_reply_done = false;
    s_output_seen = false;
    s_idle_since_us = 0;
    s_expects_reply = -1;
    s_out_tail[0] = 0;
    s_followup_turn = false;
    s_followups_left = s_max_followups;
    s_reply_deadline_us = 0;
    s_no_input_deadline_us = 0;
    s_request_deadline_us = now + TALK_STARTUP_US;
    s_retry_cold = false;
    s_t_tap = now;
    s_t_history = s_t_session = s_t_ws = s_t_listen = s_t_uplink = s_t_reply = 0;
    s_t_buffered_ms = 0;
    s_t_auth_ms = s_t_api_tls_ms = s_t_ws_tls_ms = -1;
    s_t_path = "companion";
    s_t_logged = false;
    s_t_failed = false;
    s_turn_listened = false;
    box_status_set(KBX_HOST_THINKING);
    xSemaphoreGive(s_lock);
}

/* The replayed utterance goes out as the first text frame once the Companion
 * session listens. Wiped right after, like every transcript. */
static void send_pending_text(void)
{
    static EXT_RAM_BSS_ATTR char frame[KBX_VOICE_UTTERANCE_MAX * 2U + 64U];
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool send = s_requested && s_switch_text_pending && s_media_ready && s_client &&
        s_ws_connected && !s_ws_closed && !s_suspended && s_agent_enabled &&
        !audio_is_muted() && s_session_revision == s_revision;
    uint32_t revision = s_revision;
    esp_websocket_client_handle_t client = s_client;
    bool built = true;
    int length = 0;
    if (send) {
        built = kbx_voice_text_frame(s_switch.utterance, frame, sizeof(frame));
        length = built ? (int)strlen(frame) : 0;
        memset(&s_switch, 0, sizeof(s_switch));
        s_switch_text_pending = false;
        s_reply_deadline_us = esp_timer_get_time() + SWITCH_REPLY_US;
        s_t_uplink = esp_timer_get_time();
        box_status_set(KBX_HOST_THINKING);
    }
    xSemaphoreGive(s_lock);
    bool ok = !send || (built && esp_websocket_client_send_text(client, frame, length,
        pdMS_TO_TICKS(1000)) == length);
    memset(frame, 0, sizeof(frame));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!ok && s_requested && revision == s_revision) {
        publish_locked(KBX_VOICE_ERR_SERVICE, 0, built ? "text_send" : "text_frame");
        cancel_request();
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_client_lock);
}

static void publish_ws_failure_locked(bool timeout);

static bool drain_uplink(void)
{
    int16_t frame[AUDIO_CAPTURE_16K_FRAME_SAMPLES];
    enum { SEND_NONE, SEND_PCM, SEND_END, SEND_BACKLOG } action = SEND_NONE;
    bool ok = true;
    uint32_t backlog = 0;
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t revision = s_revision;
    esp_websocket_client_handle_t client = s_client;
    bool transport_gone = !s_client || !s_ws_connected || s_ws_closed;
    if (!s_requested || !s_media_ready || !s_client || !s_ws_connected ||
        s_ws_closed || s_suspended || !s_agent_enabled || s_session_revision != s_revision ||
        audio_is_muted()) {
        ok = false;
    } else if (!s_mode_pending && !s_input_final_received && !s_reply_started && !s_reply_done &&
               !s_uplink_stopped) {
        xSemaphoreTake(s_pcm_lock, portMAX_DELAY);
        backlog = s_pcm.count;
        bool want_end = backlog == 0 && s_input_complete && !s_audio_end_sent;
        xSemaphoreGive(s_pcm_lock);
        /* Only write what the socket takes now: a Wi-Fi or edge round-trip
         * gap fills the TCP send buffer, and a blocked write would also hold
         * the client lock the downlink reader needs. Not writable: keep the
         * audio in the PSRAM FIFO and retry on the next pass. */
        bool writable = (backlog || want_end) &&
            esp_websocket_client_kbx_writable(client, 0) > 0;
        int64_t now = esp_timer_get_time();
        if ((backlog || want_end) && !writable) {
            if (!s_uplink_blocked_since_us) {
                s_uplink_blocked_since_us = now;
                ESP_LOGI(TAG, "uplink waiting: socket send buffer full (backlog %u ms)",
                         (unsigned)(backlog * 20U));
            } else if (now - s_uplink_blocked_since_us > UPLINK_BACKLOG_MAX_US ||
                       backlog > UPLINK_BACKLOG_MAX_FRAMES) {
                action = SEND_BACKLOG;
                ok = false;
            }
        } else if (writable) {
            if (s_uplink_blocked_since_us) {
                ESP_LOGI(TAG, "uplink resumed after %d ms (backlog %u ms)",
                         (int)((now - s_uplink_blocked_since_us) / 1000), (unsigned)(backlog * 20U));
                s_uplink_blocked_since_us = 0;
            }
            xSemaphoreTake(s_pcm_lock, portMAX_DELAY);
            bool have = kbx_voice_buffer_pop(&s_pcm, frame);
            xSemaphoreGive(s_pcm_lock);
            if (have) {
                action = SEND_PCM;
            } else if (want_end) {
                action = SEND_END;
                /* Reserve and publish before send: a synchronous incoming reply
                 * must retain its newer SPEAKING/DONE state after send returns. */
                s_audio_end_sent = true;
                s_reply_deadline_us = esp_timer_get_time() + TALK_REPLY_US;
                audio_play_agent_resume();
                box_status_set(KBX_HOST_THINKING);
            }
        }
    }
    xSemaphoreGive(s_lock);
    /* Keep only the outbound gate across the internal websocket mutex. A
     * DATA callback can always acquire state and finish dispatch meanwhile.
     * The socket reported room, so these writes normally return at once;
     * the timeout is only the backstop for a link that dies mid-write. */
    if (action == SEND_PCM) {
        ok = esp_websocket_client_send_bin(client, (const char *)frame,
            sizeof(frame), pdMS_TO_TICKS(UPLINK_SEND_TIMEOUT_MS)) == (int)sizeof(frame);
        if (ok) ++s_talk_frames;
        if (ok && !s_t_uplink) s_t_uplink = esp_timer_get_time();
    } else if (action == SEND_END) {
        static const char end[] = "{\"type\":\"audio_end\"}";
        ok = esp_websocket_client_send_text(client, end, sizeof(end) - 1,
            pdMS_TO_TICKS(UPLINK_SEND_TIMEOUT_MS)) == (int)(sizeof(end) - 1);
    }
    memset(frame, 0, sizeof(frame));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* A reply may have started while the selected last microphone frame
     * awaited the WS mutex. Its newer half-duplex state wins, including
     * when that obsolete send fails after the server commits the turn. */
    if (!ok && s_requested && revision == s_revision &&
        !s_input_final_received && !s_reply_started && !s_reply_done) {
        ++s_talk_faults;
        if (action == SEND_BACKLOG) {
            ESP_LOGW(TAG, "uplink backlog: socket not writable for %d ms, %u ms of audio queued; ending the turn",
                     (int)((esp_timer_get_time() - s_uplink_blocked_since_us) / 1000),
                     (unsigned)(backlog * 20U));
            publish_locked(KBX_VOICE_ERR_SERVICE, 0, "uplink_backlog");
        } else if (s_ws_closed || transport_gone) {
            /* A real disconnect: the friendly "Connection dropped" card. */
            if (!s_session_end_seen) publish_ws_failure_locked(false);
        } else if (action != SEND_NONE) {
            ESP_LOGW(TAG, "uplink send failed on a writable socket after %u frames",
                     (unsigned)s_talk_frames);
            publish_locked(KBX_VOICE_ERR_SERVICE, 0, "uplink_send");
        }
        cancel_request();
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_client_lock);
    return ok;
}

/* Caller holds s_lock; the request is still current. Why did the media
 * transport end before the turn completed? */
static void publish_ws_failure_locked(bool timeout)
{
    int status = s_ws_err_status;
    if (timeout) {
        publish_locked(KBX_VOICE_ERR_SERVICE, 0, "ws_timeout");
    } else if (status > 0) {
        /* 401/410 here mean a refused or expired ticket, not the account. */
        kbx_voice_error_kind_t kind = status == 410 ? KBX_VOICE_ERR_SERVICE :
            status == 401 ? KBX_VOICE_ERR_UNKNOWN : KBX_VOICE_ERR_NONE;
        publish_locked(kind, status, "ws_upgrade");
    } else if (!s_ws_was_connected) {
        publish_locked(net_wifi_is_connected(NULL, 0) ? KBX_VOICE_ERR_SERVICE : KBX_VOICE_ERR_OFFLINE,
                       0, "ws_connect");
    } else if (!s_session_end_seen) {
        kbx_voice_error_t e;
        kbx_voice_error_set(&e, KBX_VOICE_ERR_SERVICE, 0, "ws_closed");
        if (s_close_code) snprintf(e.reason, sizeof(e.reason), "close_%d", s_close_code);
        publish_error_locked(&e);
    }
}

static void net_ws_task(void *arg)
{
    (void)arg;
    for (;;) {
        ++s_loops;
        bool wifi = net_wifi_is_connected(NULL, 0);
        bool prov = provisioned();
        bool muted = audio_is_muted();
        bool available = s_agent_enabled && !s_suspended && prov && wifi && !muted;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool cloud = kbx_cloud_server(s_url);
        int64_t now = esp_timer_get_time();
        bool startup_late = now >= s_request_deadline_us && !s_media_ready;
        bool mode_late = s_mode_deadline_us && now >= s_mode_deadline_us;
        bool silent = s_no_input_deadline_us && !s_input_heard && !s_reply_started &&
            !s_reply_done && now >= s_no_input_deadline_us;
        bool reply_late = s_reply_deadline_us && now >= s_reply_deadline_us;
        if (s_requested && (!available || now >= s_request_deadline_us || mode_late || silent ||
            reply_late)) {
            ESP_LOGI(TAG, "talk request ended (deadline or unavailable)");
            /* Owner actions (mute, agent off, OTA) and silence are not errors. */
            if (!wifi) {
                publish_locked(KBX_VOICE_ERR_OFFLINE, 0, "wifi");
            } else if (!prov && s_agent_enabled && !s_suspended) {
                publish_locked(KBX_VOICE_ERR_SIGNIN, 0, "signed_out");
            } else if (available) {
                if (startup_late) publish_locked(KBX_VOICE_ERR_SERVICE, 0, "connect_timeout");
                else if (mode_late) publish_locked(KBX_VOICE_ERR_SERVICE, 0, "mode_timeout");
                else if (reply_late) publish_locked(KBX_VOICE_ERR_SERVICE, 0, "reply_timeout");
            }
            cancel_request();
        }
        bool want = s_requested && available;
        bool may_open = want && s_capture_started && !s_active;
        if (may_open) s_transport_idle = false;
        uint32_t attempt = s_revision;
        xSemaphoreGive(s_lock);

        /* A completed pairing opens the same follow-up window as a turn. */
        if (s_paired_seen && prov && !s_last_paired && cloud)
            s_prepare_wanted_until_us = esp_timer_get_time() + PREPARE_WINDOW_US;
        s_last_paired = prov;
        s_paired_seen = true;

        if (may_open) {
            if (open_session() == ESP_OK) {
                s_active = true;
            } else {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (s_requested && s_revision == attempt) {
                    if (s_attempt_err.kind != KBX_VOICE_ERR_NONE) publish_error_locked(&s_attempt_err);
                    else publish_locked(KBX_VOICE_ERR_UNKNOWN, 0, "session_open");
                }
                log_timing_locked(s_t_failed ? s_error.code : "cancelled");
                cancel_request();
                xSemaphoreGive(s_lock);
                memset(s_http_auth, 0, sizeof(s_http_auth));
                s_retry_cold = false;
                s_transport_idle = true;
            }
        } else if (s_active && want && s_switch_pending) {
            route_switch_to_companion();
            continue;
        } else if (s_active) {
            bool timeout = !s_ws_connected && esp_timer_get_time() > s_connect_deadline_us;
            if (!want || s_ws_closed || timeout) {
                /* A prepared ticket the server refused at upgrade: mint once
                 * more inside the same tap; the buffered speech is kept. */
                if (want && s_used_prepared && !s_start_sent && !s_retry_cold) {
                    ESP_LOGW(TAG, "prepared %s ended before start (ws status %d); minting a fresh session",
                             s_ws_was_connected ? "socket" : "ticket", s_ws_err_status);
                    close_transport();
                    s_retry_cold = true;
                    s_active = false;
                    s_ws_closed = false;
                    continue;
                }
                xSemaphoreTake(s_lock, portMAX_DELAY);
                const char *why = NULL;
                if (s_ws_closed && s_reply_done && followup_wanted_locked(&why))
                    ESP_LOGW(TAG, "follow-up skipped (%s): the server closed the session", why);
                if (want && s_requested && !s_reply_done) publish_ws_failure_locked(timeout);
                cancel_request();
                xSemaphoreGive(s_lock);
                close_session();
                s_active = false;
                s_retry_cold = false;
                s_transport_idle = true;
            } else if (s_media_ready) {
                send_pending_mode();
                send_pending_text();
                (void)drain_uplink();
                bool capture_next = false;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                /* Keep conversation context through the reply. Only its real
                 * EOF and local playback drain allow another microphone turn;
                 * the quiet tail keeps the speaker out of its own capture. */
                if (s_requested && s_media_ready && !s_ws_closed &&
                    s_reply_done && audio_play_agent_idle()) {
                    if (!s_idle_since_us) {
                        s_idle_since_us = esp_timer_get_time();
                        box_status_set(KBX_HOST_DONE);
                    }
                    int64_t quiet = esp_timer_get_time() - s_idle_since_us;
                    kbx_voice_reply_action_t action = kbx_voice_after_reply(s_conversation,
                        s_mode_pending, s_capture_running, quiet);
                    const char *why = NULL;
                    if (!s_conversation && !s_mode_pending && !s_capture_running &&
                        quiet >= KBX_VOICE_ECHO_GUARD_US && followup_wanted_locked(&why)) {
                        /* Next stage on this session and socket. The previous
                         * stage's timing line is already out (first reply);
                         * this stage gets its own, measured from now. */
                        log_timing_locked(s_t_reply ? "ok" : "ended");
                        --s_followups_left;
                        ++s_stage;
                        s_followup_turn = true;
                        capture_next = begin_capture_turn();
                        if (capture_next) {
                            int64_t now = esp_timer_get_time();
                            s_t_tap = s_t_listen = now;
                            s_t_history = s_t_session = s_t_ws = s_t_uplink = s_t_reply = 0;
                            s_t_buffered_ms = 0;
                            s_t_auth_ms = s_t_api_tls_ms = s_t_ws_tls_ms = -1;
                            s_t_path = "followup";
                            s_t_logged = false;
                            ESP_LOGI(TAG, "follow-up stage %u/%u listening (%s, silence limit=%u s)",
                                (unsigned)s_stage, (unsigned)(s_stage + s_followups_left), why,
                                (unsigned)(TALK_FOLLOWUP_NO_INPUT_US / 1000000));
                        } else {
                            ++s_talk_faults;
                            publish_locked(KBX_VOICE_ERR_UNKNOWN, 0, "microphone");
                            cancel_request();
                        }
                    } else if (action == KBX_VOICE_REPLY_CLOSE) {
                        cancel_request();
                    } else if (action == KBX_VOICE_REPLY_LISTEN) {
                        capture_next = begin_capture_turn();
                        if (capture_next) {
                            ESP_LOGI(TAG, "follow-up listening (silence limit=%u s)",
                                (unsigned)(TALK_NO_INPUT_US / 1000000));
                        } else {
                            ++s_talk_faults;
                            publish_locked(KBX_VOICE_ERR_UNKNOWN, 0, "microphone");
                            cancel_request();
                        }
                    }
                }
                xSemaphoreGive(s_lock);
                if (capture_next) xTaskNotifyGive(s_talk_task);
            }
        } else if (!s_requested) {
            warm_tick(wifi, prov, cloud);
        }
        /* No Wi-Fi modem sleep while a turn runs: with it the AP-side latency
         * (~110 ms mean, 340 ms peaks) stalled the uplink ACKs and ended turns
         * with uplink_backlog. */
        net_wifi_set_low_latency(s_requested);
        /* Drain faster than capture so startup PCM catches up in order. */
        vTaskDelay(pdMS_TO_TICKS(s_media_ready ? 5 : 20));
    }
}

static void talk_uplink_task(void *arg)
{
    (void)arg;
    int16_t frame[AUDIO_CAPTURE_16K_FRAME_SAMPLES];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_capture_running) continue;
        uint32_t revision = s_capture_revision;
        ++s_talk_starts;
        bool ok = true;
        /* First second of each capture turn: level as captured for the
         * uplink, so a silent uplink shows in the log (dev18 regression). */
        uint64_t lvl_sq = 0;
        int lvl_peak = 0;
        unsigned lvl_frames = 0;
        while (ok && s_talk_active && s_requested && revision == s_revision) {
            audio_capture_telemetry_t tel = {0};
            esp_err_t err = audio_capture_read_16k(frame,
                AUDIO_CAPTURE_MAX_READ_TIMEOUT_MS, &tel);
            if (err != ESP_OK || !tel.valid || tel.fault != AUDIO_CAPTURE_FAULT_NONE) {
                ok = false;
                break;
            }
            xSemaphoreTake(s_pcm_lock, portMAX_DELAY);
            /* Recheck after read and while holding the same lock as scrub. */
            if (s_requested && s_talk_active && revision == s_revision)
                ok = kbx_voice_buffer_push(&s_pcm, frame);
            xSemaphoreGive(s_pcm_lock);
            if (lvl_frames < 50U) {
                for (int i = 0; i < AUDIO_CAPTURE_16K_FRAME_SAMPLES; i++) {
                    int v = frame[i] < 0 ? -frame[i] : frame[i];
                    if (v > lvl_peak) lvl_peak = v;
                    lvl_sq += (uint64_t)((int64_t)frame[i] * frame[i]);
                }
                if (++lvl_frames == 50U) {
                    double rms = sqrt((double)lvl_sq / (50.0 * AUDIO_CAPTURE_16K_FRAME_SAMPLES));
                    ESP_LOGI(TAG, "uplink level first 1 s: rms=%.1f dBFS peak=%.1f dBFS",
                             rms > 0.5 ? 20.0 * log10(rms / 32768.0) : -96.0,
                             lvl_peak > 0 ? 20.0 * log10(lvl_peak / 32768.0) : -96.0);
                }
            }
            memset(frame, 0, sizeof(frame));
        }
        (void)audio_capture_stop_16k();
        memset(frame, 0, sizeof(frame));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (revision == s_revision && s_requested) {
            if (!ok) {
                ++s_talk_faults;
                ESP_LOGW(TAG, "talk capture invalid or startup buffer exhausted; cancel");
                publish_locked(KBX_VOICE_ERR_UNKNOWN, 0, "microphone");
                cancel_request();
            } else {
                s_input_complete = true;
            }
        }
        s_talk_active = false;
        cue_listen_end();
        s_capture_running = false;
        xSemaphoreGive(s_lock);
    }
}

bool net_ws_ready_for_talk(void)
{
    return s_task_started && s_agent_enabled && !s_suspended &&
        !audio_is_muted() && !s_requested && !s_capture_running &&
        s_transport_idle && provisioned() && net_wifi_is_connected(NULL, 0);
}

bool net_ws_talk_active(void)
{
    return s_talk_active;
}

/* Local capture counts as listening (the PSRAM FIFO keeps every word until
 * the server acknowledges). Connecting is shown only while a governed mode
 * change is pending or a finished utterance still waits for media. */
bool net_ws_talk_connecting(void)
{
    return s_requested && ((!s_media_ready && !s_talk_active) || s_mode_pending);
}

void net_ws_talk_cancel(void)
{
    if (!s_lock) return;
    control_lock();
    cancel_request();
    downlink_reset();
    audio_play_agent_flush();
    box_status_set(KBX_HOST_IDLE);
    control_unlock();
}

static bool start_talk_request(bool conversation, uint32_t wake_mute_epoch)
{
    /* A tap or wake hit: allow one follow-up turn. */
    if (!s_lock) return false;
    if (!net_ws_ready_for_talk()) return false;
    control_lock();
    /* Serialize with suspend/off and reject another request until cleanup. */
    if (!s_agent_enabled || s_suspended || s_requested || s_capture_running ||
        !s_transport_idle || audio_is_muted() ||
        (wake_mute_epoch && !audio_observer_epoch_current(wake_mute_epoch)) ||
        (conversation && !kbx_cloud_server(s_url))) {
        control_unlock();
        return false;
    }
    /* 12 KB: with echo cancellation the uplink task runs ESP-SR's AEC
     * (audio_capture_read_16k), which overflowed 4 KB. PSRAM stack. */
    if (!s_talk_task && xTaskCreateWithCaps(talk_uplink_task, "kbx_va_up", 12288,
        NULL, 6, &s_talk_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_talk_task = NULL;
        control_unlock();
        return false;
    }
    ++s_revision;
    s_media_ready = false;
    s_followup_turn = false;
    s_followups_left = s_max_followups;
    s_stage = 0;
    s_uplink_blocked_since_us = 0;
    s_close_code = 0;
    int64_t tap = esp_timer_get_time();
    if (!begin_capture_turn()) {
        control_unlock();
        return false;
    }
    /* The detector's last frame must belong to the same unmuted epoch as
     * this capture. A mute/unmute while acquiring capture cannot revive an
     * old hit; nothing has been published to the network worker yet. */
    if (wake_mute_epoch && !audio_observer_epoch_current(wake_mute_epoch)) {
        (void)audio_capture_stop_16k();
        s_capture_running = false;
        s_capture_started = false;
        s_talk_active = false;
        cue_listen_end();
        clear_pcm(false);
        box_status_set(KBX_HOST_IDLE);
        control_unlock();
        return false;
    }
    /* A new tap is the retry: clear the previous failure and timing. */
    memset(&s_error, 0, sizeof(s_error));
    caption_reset();
    s_t_tap = tap;
    s_t_history = s_t_session = s_t_ws = s_t_listen = s_t_uplink = s_t_reply = 0;
    s_t_buffered_ms = 0;
    s_t_auth_ms = s_t_api_tls_ms = s_t_ws_tls_ms = -1;
    s_t_path = "cold";
    s_t_logged = false;
    s_t_failed = false;
    s_turn_listened = false;
    s_retry_cold = false;
    s_requested = true;
    s_conversation = false;
    if (conversation) queue_conversation_mode(true);
    control_unlock();
    xTaskNotifyGive(s_talk_task);
    return true;
}

bool net_ws_talk_start(void)
{
    return start_talk_request(false, 0);
}

bool net_ws_talk_start_for_wakeword(uint32_t mute_epoch)
{
    return mute_epoch != 0 && start_talk_request(false, mute_epoch);
}

bool net_ws_talk_barge_in(uint32_t mute_epoch)
{
    if (mute_epoch == 0) return false;
    /* Stop the reply at once, then take the next turn as soon as the
     * session teardown leaves the transport idle (bounded). */
    net_ws_talk_cancel();
    for (int i = 0; i < 40; ++i) {
        if (start_talk_request(false, mute_epoch)) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

bool net_ws_talk_toggle(void)
{
    if (!s_lock) return false;
    control_lock();
    if (s_requested && (s_conversation || s_mode_pending)) {
        cancel_request();
        downlink_reset();
        audio_play_agent_flush();
        box_status_set(KBX_HOST_IDLE);
        control_unlock();
        return false;
    }
    if (s_requested && !s_talk_active && !s_media_ready) {
        /* Third tap while the committed turn still connects: abort it. */
        cancel_request();
        box_status_set(KBX_HOST_IDLE);
        control_unlock();
        return false;
    }
    if (s_talk_active) {
        s_talk_active = false;
        cue_listen_end();
        /* The second tap commits the turn, also while media still connects:
         * the display already said Listening, so the buffered words are sent
         * once the server acknowledges, followed by audio_end. */
        if (audio_is_muted()) {
            cancel_request();
            box_status_set(KBX_HOST_IDLE);
        }
        control_unlock();
        return false;
    }
    control_unlock();
    return start_talk_request(false, 0);
}

bool net_ws_conversation_start(void)
{
    if (!s_lock) return false;
    control_lock();
    if (!s_route_agent) {
        /* The direct voice agent keeps the dialogue itself; a conversation
         * is an ordinary turn without governed conversation control. */
        control_unlock();
        return s_requested ? true : start_talk_request(false, 0);
    }
    if (s_requested && s_agent_enabled && !s_suspended && !audio_is_muted() && kbx_cloud_server(s_url)) {
        if (!s_conversation && !s_mode_pending) queue_conversation_mode(false);
        control_unlock();
        return true;
    }
    control_unlock();
    return start_talk_request(true, 0);
}

bool net_ws_conversation_active(void)
{
    return s_requested && (s_conversation || s_mode_pending);
}

bool net_ws_route_switch_active(void)
{
    return s_requested && s_switch_active;
}

/* Test console (KBX frontdoor ...): feeds a server receipt for the CURRENT
 * session through the same parser and state path as the socket, so the
 * switch and mode handling can be proven on a device before the server sends
 * them. Does nothing without an active cloud session; a real receipt is
 * checked against the same session id. */
bool net_ws_debug_receipt(const char *kind, const char *arg)
{
    if (!s_lock || !kind || !arg || !arg[0]) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    char request_id[48];
    new_request_id(request_id, sizeof(request_id));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_requested && FRONT_DOOR_SESSION() && s_session_id[0] && !s_ws_closed;
    if (ok && !strcmp(kind, "route")) {
        cJSON_AddStringToObject(root, "type", "event");
        cJSON_AddStringToObject(root, "event_type", "route_switch");
        cJSON *route = cJSON_AddObjectToObject(root, "route");
        cJSON_AddNumberToObject(route, "version", 1);
        cJSON_AddStringToObject(route, "to", "agent");
        cJSON_AddStringToObject(route, "voice_session_id", s_session_id);
        cJSON_AddStringToObject(route, "request_id", request_id);
        cJSON_AddStringToObject(route, "reason", "explicit_companion");
        cJSON_AddStringToObject(route, "utterance", arg);
    } else if (ok && !strcmp(kind, "mode")) {
        cJSON_AddStringToObject(root, "type", "event");
        cJSON_AddStringToObject(root, "event_type", "conversation_mode");
        cJSON *receipt = cJSON_AddObjectToObject(root, "conversation");
        cJSON_AddNumberToObject(receipt, "version", 1);
        cJSON_AddStringToObject(receipt, "mode", arg);
        cJSON_AddStringToObject(receipt, "status", "accepted");
        cJSON_AddStringToObject(receipt, "voice_session_id", s_session_id);
        cJSON_AddStringToObject(receipt, "conversation_session_id", s_session_id);
        cJSON_AddStringToObject(receipt, "request_id", request_id);
    } else {
        ok = false;
    }
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    const bool fed = text != NULL;
    if (fed) handle_ws_text(text, strlen(text));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "test receipt %s: %s", kind, fed ? "fed" : "ignored (no active cloud session)");
    free(text);
    return fed;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t net_ws_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_client_lock) {
        s_client_lock = xSemaphoreCreateMutex();
    }
    if (!s_pcm_lock) {
        s_pcm_lock = xSemaphoreCreateMutex();
    }
    {
        nvs_handle_t h;
        uint8_t v = FOLLOWUPS_DEFAULT;
        if (nvs_open("kbx_assist", NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_u8(h, "maxfu_v1", &v);
            nvs_close(h);
        }
        s_max_followups = v <= FOLLOWUPS_MAX ? v : FOLLOWUPS_DEFAULT;
        uint8_t voice = NET_WS_VOICE_AUTO;
        if (nvs_open("kbx_assist", NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_u8(h, "voice_v1", &voice);
            nvs_close(h);
        }
        s_voice_setting = voice < NET_WS_VOICE_COUNT ? voice : NET_WS_VOICE_AUTO;
    }
    if (!s_lock || !s_client_lock || !s_pcm_lock) {
        return ESP_ERR_NO_MEM;
    }
    nvs_load();
    if (s_url[0]) {
        ESP_LOGI(TAG, "server from NVS: %s (token %s)", s_url, s_token[0] ? "set" : "missing");
    } else {
        ESP_LOGI(TAG, "no server provisioned (KBX server <url> / KBX token <bearer>)");
    }

    /* The task stack lives in INTERNAL RAM, which can be tight after
     * Wi-Fi/BT/audio init. Earlier fixed 12288 failed silently here (task
     * never ran -> provisioning never applied). Hence: log heap + fallback
     * ladder + s_task_started for KBX net?. Large buffers are file-static;
     * the stack only has to carry the mbedTLS handshake + small locals. */
    ESP_LOGI(TAG, "init: internal-heap free=%u B, largest block=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* Task stack in PSRAM (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y):
     * internal RAM is too tight after LVGL+Wi-Fi+BT for a ~12 KB stack.
     * Blocking TLS work (esp_http_client POST/DELETE) runs in this task;
     * mbedTLS heap lives in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC).
     * 16 KB stack because PSRAM is plentiful. Internal fallback if the
     * PSRAM stack fails. */
    if (xTaskCreateWithCaps(net_ws_task, "net_ws", 16384, NULL, 5, NULL, MALLOC_CAP_SPIRAM) == pdPASS) {
        s_task_started = true;
        ESP_LOGI(TAG, "net_ws task started (PSRAM stack 16384)");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "PSRAM stack failed - trying internal RAM");
    if (xTaskCreate(net_ws_task, "net_ws", 8192, NULL, 5, NULL) == pdPASS) {
        s_task_started = true;
        ESP_LOGW(TAG, "net_ws task started with internal stack 8192 (tight)");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "net_ws task failed to start - not enough RAM");
    return ESP_FAIL;
}

esp_err_t net_ws_set_server(const char *url)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE; /* boot window: CDC before net_ws_init */
    }
    if (!url || !url[0] || strlen(url) >= sizeof(s_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    char echo[NET_WS_URL_MAX];
    control_lock();
    cancel_request();
    ++s_prov_epoch;
    strlcpy(s_url, url, sizeof(s_url));
    size_t l = strlen(s_url);
    while (l > 0 && s_url[l - 1] == '/') {
        s_url[--l] = '\0';
    }
    nvs_store(NET_NVS_KEY_URL, s_url);
    strlcpy(echo, s_url, sizeof(echo));
    control_unlock();
    ESP_LOGI(TAG, "server active: %s", echo);
    return ESP_OK;
}

esp_err_t net_ws_set_token(const char *bearer)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bearer || !bearer[0] || strlen(bearer) >= sizeof(s_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    control_lock();
    cancel_request();
    ++s_prov_epoch;
    strlcpy(s_token, bearer, sizeof(s_token));
    size_t len = strlen(s_token);
    nvs_store(NET_NVS_KEY_TOKEN, s_token);
    control_unlock();
    ESP_LOGI(TAG, "token active (len=%u)", (unsigned)len);
    return ESP_OK;
}

esp_err_t net_ws_set_provider(const char *name)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (name && strlen(name) >= NET_WS_PROVIDER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    control_lock();
    cancel_request();
    strlcpy(s_provider, name ? name : "", sizeof(s_provider));
    nvs_store(NET_NVS_KEY_PROVIDER, s_provider);
    control_unlock();
    ESP_LOGI(TAG, "provider: %s", (name && name[0]) ? name : "(server-default)");
    return ESP_OK;
}

esp_err_t net_ws_clear(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    control_lock();
    s_agent_enabled = false;
    cancel_request();
    ++s_prov_epoch;
    audio_play_agent_flush();
    s_url[0] = '\0';
    s_token[0] = '\0';
    nvs_store(NET_NVS_KEY_URL, "");
    nvs_store(NET_NVS_KEY_TOKEN, "");
    control_unlock();
    ESP_LOGI(TAG, "provisioning cleared");
    return ESP_OK;
}

void net_ws_suspend(void)
{
    control_lock();
    s_suspended = true;
    if (s_pcm_lock) cancel_request();
    audio_play_agent_flush();
    s_ws_closed = true;
    control_unlock();
}

void net_ws_resume(void)
{
    /* Keep the owner's agent preference, including a logout during the
     * update check. Resuming a temporary pause must never turn it on. */
    control_lock();
    s_suspended = false;
    control_unlock();
}

bool net_ws_is_suspended_idle(void)
{
    /* Includes the warm path: no prepared ticket, no kept-alive TLS. */
    return s_suspended && s_transport_idle && !s_active && !s_bg_busy && !s_prepared &&
        !s_predial && !s_api_open;
}

void net_ws_warm_hold(bool hold)
{
    if (s_warm_hold != hold) ESP_LOGI(TAG, "warm: %s for the settings sync", hold ? "held" : "released");
    s_warm_hold = hold;
}

bool net_ws_warm_released(void)
{
    return s_warm_hold && !s_bg_busy && !s_prepared && !s_predial && !s_api_open;
}

esp_err_t net_ws_set_agent_enabled(bool enabled)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    control_lock();
    bool cloud = kbx_cloud_server(s_url);
    control_unlock();
    if (enabled && !cloud && !provisioned()) {
        ESP_LOGW(TAG, "agent on ignored - not provisioned (KBX server/token first)");
        return ESP_ERR_INVALID_STATE;
    }
    control_lock();
    s_agent_enabled = enabled;
    if (!enabled) {
        /* Stop media locally now; teardown may wait for an in-flight TLS
         * request. Its result is fenced before a provider can be started. */
        cancel_request();
        audio_play_agent_flush();
        s_ws_closed = true;
    }
    control_unlock();
    nvs_handle_t h;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NET_NVS_KEY_AGENT, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "agent %s (persisted)", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t net_ws_use_cloud(void)
{
    esp_err_t err = net_ws_set_server(KBX_CLOUD_SERVER);
    if (err != ESP_OK) return err;
    control_lock();
    memset(s_token, 0, sizeof(s_token));
    ++s_prov_epoch;
    nvs_store(NET_NVS_KEY_TOKEN, "");
    control_unlock();
    (void)net_ws_set_provider(NULL);
    return net_ws_set_agent_enabled(true);
}

esp_err_t net_ws_set_max_followups(unsigned n)
{
    if (n > FOLLOWUPS_MAX) return ESP_ERR_INVALID_ARG;
    s_max_followups = (uint8_t)n;
    nvs_handle_t h;
    if (nvs_open("kbx_assist", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "maxfu_v1", (uint8_t)n);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "assist: max follow-ups %u", n);
    return ESP_OK;
}

unsigned net_ws_max_followups(void)
{
    return s_max_followups;
}

const char *net_ws_voice_id(net_ws_voice_t voice)
{
    return (unsigned)voice < NET_WS_VOICE_COUNT ? k_voice_ids[voice] : NULL;
}

net_ws_voice_t net_ws_voice(void)
{
    return (net_ws_voice_t)s_voice_setting;
}

esp_err_t net_ws_set_voice_id(const char *id)
{
    for (unsigned i = 0; id && i < NET_WS_VOICE_COUNT; i++) {
        if (strcmp(id, k_voice_ids[i]) != 0) continue;
        s_voice_setting = (uint8_t)i;
        nvs_handle_t h;
        if (nvs_open("kbx_assist", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "voice_v1", (uint8_t)i);
            nvs_commit(h);
            nvs_close(h);
        }
        net_ws_log_voice();
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

void net_ws_log_voice(void)
{
    const char *voice = speech_voice();
    ESP_LOGI(TAG, "assist: voice %s -> %s for %s (phrase %s; KBX voice auto|female|male)",
             k_voice_ids[s_voice_setting % NET_WS_VOICE_COUNT], voice ? voice : "server-default",
             speech_locale(), wakeword_phrase_id(wakeword_get_phrase()));
}

bool net_ws_request_active(void)
{
    return s_requested;
}

bool net_ws_tls_busy(void)
{
    return s_tls_api || s_tls_ws;
}

bool net_ws_warm_ready(void)
{
    return !s_bg_busy && (s_prepared || s_warm_history[0]);
}

bool net_ws_last_error(kbx_voice_error_t *out, uint32_t *age_ms)
{
    if (!s_lock || !out) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_error.kind != KBX_VOICE_ERR_NONE;
    *out = s_error;
    if (age_ms) *age_ms = (uint32_t)((esp_timer_get_time() - s_error_us) / 1000);
    xSemaphoreGive(s_lock);
    return has;
}

kbx_voice_error_kind_t net_ws_unready_reason(void)
{
    if (!net_wifi_is_connected(NULL, 0)) return KBX_VOICE_ERR_OFFLINE;
    if (!s_lock) return KBX_VOICE_ERR_NONE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool cloud = kbx_cloud_server(s_url);
    xSemaphoreGive(s_lock);
    if (cloud) {
        cloud_auth_status_t st;
        cloud_auth_get_status(&st);
        if (!st.paired) return KBX_VOICE_ERR_SIGNIN;
    }
    return KBX_VOICE_ERR_NONE;
}

bool net_ws_agent_enabled(void)
{
    return s_agent_enabled;
}

void net_ws_set_route_agent(bool agent)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_route_agent = agent;
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NET_NVS_KEY_ROUTE, agent ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "voice route: %s (saved)", agent ? "agent" : "fast");
}

bool net_ws_route_agent(void)
{
    return s_route_agent;
}

bool net_ws_is_connected(void)
{
    return s_ws_connected;
}

void net_ws_log_status(void)
{
    if (!s_lock) {
        ESP_LOGI(TAG, "status: not initialized yet");
        return;
    }
    char ip[16];
    bool wifi = net_wifi_is_connected(ip, sizeof(ip));
    char urlbuf[NET_WS_URL_MAX];
    char sid[64];
    size_t tlen;
    char provbuf[NET_WS_PROVIDER_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(urlbuf, s_url[0] ? s_url : "(unset)", sizeof(urlbuf));
    strlcpy(sid, s_session_id[0] ? s_session_id : "-", sizeof(sid));
    strlcpy(provbuf, s_provider[0] ? s_provider : "(server-default)", sizeof(provbuf));
    tlen = strlen(s_token);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "status: server=%s provider=%s token=%s agent=%s wifi=%s session=%s connected=%s",
             urlbuf, provbuf,
             tlen ? "set" : "(unset)",
             s_agent_enabled ? "on" : "off",
             wifi ? ip : "(not connected)",
             sid,
             s_ws_connected ? "yes" : "no");
    ESP_LOGI(TAG, "  diag: task=%s loops=%u int=%u spiram=%u (token-len=%u, value never logged)",
             s_task_started ? "running" : "NOT-STARTED",
             (unsigned)s_loops,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)tlen);
    ESP_LOGI(TAG, "  route: %s", s_route_agent ? "agent" : "fast");
    ESP_LOGI(TAG, "  warm: api=%s history=%s prepared=%s (%d s left)%s last_error=%s/%s",
             s_api_open ? (s_api_warm ? "kept-alive" : "open") : "closed",
             s_warm_history[0] ? "ready" : "none", s_prepared ? "yes" : "no",
             s_prepared ? (int)((s_prepared_until_us - esp_timer_get_time()) / 1000000) : 0,
             s_warm_hold ? " held=settings-sync" : "",
             kbx_voice_error_name(s_error.kind), s_error.kind ? s_error.code : "-");
    ESP_LOGI(TAG, "  talk: active=%s mode=%s phases=%u frames=%u faults=%u",
             s_talk_active ? "yes" : "no", s_conversation ? "conversation" : "one-shot",
             (unsigned)s_talk_starts,
             (unsigned)s_talk_frames, (unsigned)s_talk_faults);
}
