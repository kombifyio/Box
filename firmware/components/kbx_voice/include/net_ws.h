/*
 * kombify box - net_ws: standalone voice-agent client against speechkit-server.
 *
 * Standalone touch-to-talk builds the cloud speech pipeline without a PC.
 * Operator provisioning uses the same CDC channel as Wi-Fi:
 *   KBX server <url>     speechkit-server base URL (e.g. https://host)
 *   KBX server?          status into the log
 *   KBX token <bearer>   service bearer for session-create (never logged)
 *   KBX token?           only "set (len=NN)", never the value
 *   KBX agent on|off|?   arm/disarm Talk (no idle provider session)
 *
 * Contract (verified against SpeechKit
 * internal/server/voiceagent + pkg/speechkit/client):
 *   POST <server>/v1/voiceagent/sessions  (Bearer) -> {session_id, ws_url,
 *     ws_subprotocol="ticket.<t>", ticket, expires_at}; ticket TTL 90 s.
 *   WS upgrade on ws_url with Sec-WebSocket-Protocol=ws_subprotocol only.
 *   First WS frame (text/JSON): {"type":"start","media_transport":"websocket",
 *     "provider":"deepgram"}. Then state:listening.
 *   Server skips Origin enforcement only when SPEECHKIT_ALLOW_EMPTY_ORIGIN=1.
 *   Audio (M1.3+): 16 kHz S16LE mono up, 24 kHz down, frame <= 64 KiB.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "voice_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load provisioning from NVS (namespace "net", keys sk_url/sk_token) and
 * start the lifecycle task. Does not dial on its own: only an explicit Talk
 * request opens a session after the agent is armed. Missing
 * provisioning is not an error (box stays USB-only as before). Agent-on
 * only arms Talk; an explicit tap opens the media session. */
esp_err_t net_ws_init(void);

/* Select the account-authenticated Cloud path. No shared service credential
 * is retained; the cloud_auth module owns pairing and renewal. */
esp_err_t net_ws_use_cloud(void);

/* Persist the server base URL in NVS. Tears down the active session so the
 * next connection uses the new URL. */
esp_err_t net_ws_set_server(const char *url);

/* Persist the bearer in NVS. NEVER logged or printed. */
esp_err_t net_ws_set_token(const char *bearer);

/* Voice-agent provider for the start frame (NVS sk_provider). NULL/empty =
 * server default (recommended). Ends a live session so the next one uses
 * the new provider. */
esp_err_t net_ws_set_provider(const char *name);

/* Clear server URL + token from NVS and end a live session. */
esp_err_t net_ws_clear(void);

/* Agent on/off. On arms Talk while paired + Wi-Fi up. Off cancels capture
 * and closes any session cleanly (stop + DELETE). No idle provider work. */
esp_err_t net_ws_set_agent_enabled(bool enabled);
bool net_ws_agent_enabled(void);
/* Voice route for a cloud-paired box: false = fast direct voice agent
 * (default), true = governed Workbench agent with Assist history. */
void net_ws_set_route_agent(bool agent);
bool net_ws_route_agent(void);
/* A server tool of the Voice Agent runs for the current turn (shown as a
 * working state); name gets the tool's name. */
bool net_ws_tool_activity(char *name, size_t capacity);

/* Drop the live session without persisting agent-off. Used by OTA so the
 * reboot still restores the previous NVS agent flag. */
void net_ws_suspend(void);
/* End only the temporary OTA pause; preserve the stored/user agent setting. */
void net_ws_resume(void);
/* True after all session/TLS cleanup has drained following suspend. */
bool net_ws_is_suspended_idle(void);

/* true while a WS session is connected. For UI/diagnostics. */
bool net_ws_is_connected(void);

/* Talk (M1.3, toggle model): tap starts a talk phase (mic 16 kHz uplink,
 * server VAD commits turns), another tap ends it (audio_end).
 * ready = armed + paired + Wi-Fi + not muted + previous cleanup complete.
 * First tap captures immediately into a bounded PSRAM FIFO while connecting;
 * no PCM is sent until the real server listening acknowledgement. A tap
 * during connection cancels and scrubs; during listening it commits input.
 * Default is one-shot. Explicit conversation mode resumes listening after
 * reply EOF and speaker drain; a tap then cancels the whole conversation.
 * Fifteen seconds without speech ends it; agent-on alone never opens media.
 * toggle returns the NEW recording state (false also when refused). */
bool net_ws_ready_for_talk(void);
bool net_ws_talk_active(void);
/* Capture starts at the tap into the PSRAM FIFO, so the UI shows Listening
 * at once. Connecting is true only while a governed conversation request
 * awaits its receipt or a committed utterance still awaits media. A second
 * tap commits the utterance (also before media is ready); a further tap
 * while it still connects cancels. */
bool net_ws_talk_connecting(void);
/* Start-only trigger for local activation. A concurrent tap or request is
 * refused rather than toggling or committing that other capture. */
bool net_ws_talk_start(void);
/* Local wake word: same start-only path as a tap (pre-dial included). The
 * detector supplies its final frame's nonzero mute epoch, revalidated
 * around capture acquisition before any network request is published. */
bool net_ws_talk_start_for_wakeword(uint32_t mute_epoch);
/* Wake word during a reply (full duplex): stop playback and the request,
 * then start a new listening turn; waits up to 2 s for the teardown. */
bool net_ws_talk_barge_in(uint32_t mute_epoch);
bool net_ws_talk_toggle(void);
/* Queue explicit conversation entry, also usable during a one-shot request.
 * Only the server's scoped acknowledgement enables it. active includes a
 * pending request so tapping Stop can cancel it. No mode survives teardown. */
bool net_ws_conversation_start(void);
bool net_ws_conversation_active(void);
/* The server handed this turn to the Companion (front door route_switch):
 * the answer comes from the registered agent route and takes longer. The
 * saved route setting is not touched; the next tap starts on it again. */
bool net_ws_route_switch_active(void);
/* Test console: feed a receipt of kind "route" (arg = utterance) or "mode"
 * (arg = oneshot|conversation) for the current session through the socket's
 * own handler. False without an active cloud session. */
bool net_ws_debug_receipt(const char *kind, const char *arg);
/* Cancel this finite request, including queued input and playback. Late
 * provider frames stay fenced; account pairing and agent preference remain. */
void net_ws_talk_cancel(void);

/* Last failure of a tap (first cause wins), cleared by the next tap.
 * Returns false when none. Contains server codes only, never credentials. */
bool net_ws_last_error(kbx_voice_error_t *out, uint32_t *age_ms);
/* A talk request is in flight (capture, thinking, reply): background
 * network work such as the edge device sync stays off. A prepared idle
 * session does not count. */
bool net_ws_request_active(void);
/* A TLS handshake (API or WebSocket) is running; the UI holds heavy redraws
 * so PSRAM/cache bandwidth goes to the handshake. */
bool net_ws_tls_busy(void);
/* The idle warm path has settled: a prepared session or a prefetched
 * history is waiting and no background request is running. Read-only. */
bool net_ws_warm_ready(void);
/* Follow-up stages per tap or wake hit (0-3, default 3), persisted. */
esp_err_t net_ws_set_max_followups(unsigned n);
unsigned net_ws_max_followups(void);
/* Reply voice gender (assist.voice, KBX voice), persisted. auto: male for the
 * wake phrases jarvis, hey_jarvis, mycroft and computer, female otherwise.
 * de and en send an Aura-2 voice id in start; other languages none. */
typedef enum { NET_WS_VOICE_AUTO = 0, NET_WS_VOICE_FEMALE, NET_WS_VOICE_MALE, NET_WS_VOICE_COUNT } net_ws_voice_t;
const char *net_ws_voice_id(net_ws_voice_t voice);
net_ws_voice_t net_ws_voice(void);
esp_err_t net_ws_set_voice_id(const char *id);
void net_ws_log_voice(void);
/* Why a tap would be refused right now: OFFLINE (no Wi-Fi), SIGNIN (cloud
 * not paired) or NONE (other or ready). */
kbx_voice_error_kind_t net_ws_unready_reason(void);

/* Display caption: latest transcript of the current turn (input while the
 * user speaks, output while the agent answers). Cleared by the next tap.
 * seq changes whenever the text does. Returns false when there is none. */
typedef enum {
    NET_WS_CAPTION_NONE = 0,
    NET_WS_CAPTION_INPUT,
    NET_WS_CAPTION_OUTPUT,
} net_ws_caption_kind_t;
bool net_ws_caption(char *out, size_t capacity, net_ws_caption_kind_t *kind, bool *final,
                    uint32_t *seq);

/* Log status to the CDC log (server set? token set+length? agent? Wi-Fi
 * IP? session id? state?). NEVER prints the token value. */
void net_ws_log_status(void);

#ifdef __cplusplus
}
#endif
