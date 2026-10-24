/*
 * kombify box C6 - local wake word.
 *
 * The S3 Box's microWakeWord engine (firmware/main/wakeword_mww.cc) with the
 * same models: Kombify, Hey Kombify, Hey Kubi (kombify-trained) and the stock
 * Hey Jarvis. While the box is idle and paired, a task reads the microphone
 * and feeds the model; a hit starts a voice turn exactly like a tap. During a
 * turn (or any other microphone user) the engine is unloaded so the TLS
 * handshakes keep their RAM, and it is rebuilt afterwards.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    C6_WAKE_OFF = -1,
    C6_WAKE_KOMBIFY = 0,
    C6_WAKE_HEY_KOMBIFY,
    C6_WAKE_HEY_KUBI,
    C6_WAKE_HEY_JARVIS,
    C6_WAKE_COUNT,
} c6_wake_phrase_t;

esp_err_t c6_wake_init(void);
/* Selected phrase (persisted); C6_WAKE_OFF disables the engine. */
void c6_wake_set_phrase(c6_wake_phrase_t phrase);
c6_wake_phrase_t c6_wake_phrase(void);
const char *c6_wake_phrase_name(c6_wake_phrase_t phrase);
/* Release the microphone and the model RAM; blocks until the task parked.
 * Pauses nest; each pause needs one resume. */
void c6_wake_pause(void);
void c6_wake_resume(void);
/* True while the engine listens for the phrase. */
bool c6_wake_listening(void);
void c6_wake_log_status(void);
