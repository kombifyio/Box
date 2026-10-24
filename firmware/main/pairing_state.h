#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PAIRING_AUTHORIZATION_WINDOW_US (30LL * 1000LL * 1000LL)
#define PAIRING_STAGING_WINDOW_US (5LL * 60LL * 1000LL * 1000LL)

typedef enum {
    PAIRING_STATE_CLOSED = 0,
    PAIRING_STATE_PHYSICALLY_AUTHORIZED,
    PAIRING_STATE_STAGING,
} pairing_phase_t;

typedef struct {
    pairing_phase_t phase;
    int64_t deadline_us;
    bool rtc_verified;
} pairing_state_t;

void pairing_state_init(pairing_state_t *state);
bool pairing_state_authorize(pairing_state_t *state, int64_t now_us, bool busy);
bool pairing_state_begin(pairing_state_t *state, int64_t now_us, bool busy);
bool pairing_state_staging_open(pairing_state_t *state, int64_t now_us, bool busy);
bool pairing_state_authorization_open(pairing_state_t *state, int64_t now_us);
void pairing_state_mark_rtc_verified(pairing_state_t *state);
bool pairing_state_can_commit(pairing_state_t *state,
                              int64_t now_us,
                              bool busy,
                              bool record_complete);
void pairing_state_close(pairing_state_t *state);
