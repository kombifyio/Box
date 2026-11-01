/*
 * kombify box C6 - Cloud voice turn with conversation mode.
 *
 * Same Gateway contract as the S3 Box (docs/stack-integration.md): the paired
 * account's access token mints a session at
 * https://api.kombify.io/v1/speechkit/voiceagent/sessions, the single-use
 * ticket alone authorizes the WSS upgrade, 16 kHz S16LE mono goes up and
 * 24 kHz S16LE mono comes back. Without PSRAM the turn is half-duplex and
 * capture starts at the server's listening acknowledgement; nothing is
 * buffered before it. A tap starts a turn, a tap while listening ends the
 * utterance, any other tap cancels. A reply that expects an answer
 * (turn_end expects_reply) continues on the same session; any other reply
 * closes it.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    C6_VOICE_IDLE = 0,
    C6_VOICE_CONNECTING,
    C6_VOICE_LISTENING,
    C6_VOICE_THINKING,
    C6_VOICE_SPEAKING,
    C6_VOICE_ERROR,
} c6_voice_state_t;

esp_err_t c6_voice_init(void);
void c6_voice_tap(void);
/* Wake word hit: start a turn if idle (never toggles or cancels). phrase is
 * the device phrase id and score the detector's window mean; both go to the
 * server's second-stage check. */
void c6_voice_wake(const char *phrase, float score);
/* True once a wake turn met a server that verifies wake words; the engine
 * then nominates candidates at its lower cutoff. */
bool c6_voice_server_verifies(void);
/* Wake engine, while idle: append 20 ms to the 1.5 s pre-roll. */
void c6_voice_preroll_push(const int16_t *pcm);
c6_voice_state_t c6_voice_state(void);
/* The Companion holds the current turn (spoken Companion request, front door
 * route_switch): its answer takes longer than the quick one. */
bool c6_voice_companion(void);
/* Test console: feed a front-door receipt of kind "route" (arg = utterance)
 * or "mode" (arg = oneshot|conversation) for the open session. */
bool c6_voice_debug_receipt(const char *kind, const char *arg);
/* Short display code of the last failure (empty when none). */
void c6_voice_error(char *out, size_t capacity);
/* Latest transcript of the current turn; seq changes with the text. */
bool c6_voice_caption(char *out, size_t capacity, bool *from_agent, uint32_t *seq);
/* Speech locale for the start frame, e.g. "de-DE"; persisted. */
void c6_voice_set_locale(const char *locale);
const char *c6_voice_locale(void);
void c6_voice_log_status(void);
/* Conversation mode: follow-up turns after a reply that expects an answer,
 * 0..3 per tap or wake hit (account setting assist.max_followups; persisted). */
esp_err_t c6_voice_set_max_followups(unsigned n);
unsigned c6_voice_max_followups(void);
/* 0 during the tap or wake turn, 1.. in follow-up turns of the same session. */
unsigned c6_voice_stage(void);
/* Gateway request on the voice TLS transport (resumes its ticket). Voice task
 * only, while idle upkeep holds the account token. extra: complete header
 * lines ending in CRLF or NULL. Returns the HTTP status or 0; *resp is the
 * body (NUL terminated, valid until the next call) and *etag the response
 * ETag or "", *poll_after the x-kombify-poll-after seconds (0 when absent). */
int c6_voice_gateway_call(const char *method, const char *url, const char *extra, const char *body,
                          const char **resp, const char **etag, int *poll_after);
/* The response buffer as scratch space between Gateway calls. */
char *c6_voice_scratch(size_t *capacity);

/* Operator test path (KBX inject): while active, the uplink reads 16 kHz
 * S16LE mono from this stream instead of the microphone. */
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
StreamBufferHandle_t c6_voice_inject_begin(bool wake);
/* The test stream for the wake engine (NULL unless injected with "wake"). */
StreamBufferHandle_t c6_voice_inject_stream(void);
void c6_voice_inject_end(void);
/* The running test stream (NULL when none): the clips of follow-up turns. */
StreamBufferHandle_t c6_voice_inject_stream_active(void);
