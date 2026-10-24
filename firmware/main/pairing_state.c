#include "pairing_state.h"

#include <stddef.h>

static void expire(pairing_state_t *state, int64_t now_us)
{
    if (state && state->phase != PAIRING_STATE_CLOSED &&
        now_us >= state->deadline_us) {
        pairing_state_close(state);
    }
}

void pairing_state_init(pairing_state_t *state)
{
    pairing_state_close(state);
}

bool pairing_state_authorize(pairing_state_t *state, int64_t now_us, bool busy)
{
    if (!state || busy) {
        return false;
    }
    expire(state, now_us);
    if (state->phase != PAIRING_STATE_CLOSED) {
        return false;
    }
    state->phase = PAIRING_STATE_PHYSICALLY_AUTHORIZED;
    state->deadline_us = now_us + PAIRING_AUTHORIZATION_WINDOW_US;
    state->rtc_verified = false;
    return true;
}

bool pairing_state_begin(pairing_state_t *state, int64_t now_us, bool busy)
{
    if (!state || busy) {
        return false;
    }
    expire(state, now_us);
    if (state->phase != PAIRING_STATE_PHYSICALLY_AUTHORIZED) {
        return false;
    }
    state->phase = PAIRING_STATE_STAGING;
    state->deadline_us = now_us + PAIRING_STAGING_WINDOW_US;
    state->rtc_verified = false;
    return true;
}

bool pairing_state_staging_open(pairing_state_t *state, int64_t now_us, bool busy)
{
    if (!state || busy) {
        return false;
    }
    expire(state, now_us);
    return state->phase == PAIRING_STATE_STAGING;
}

bool pairing_state_authorization_open(pairing_state_t *state, int64_t now_us)
{
    if (!state) {
        return false;
    }
    expire(state, now_us);
    return state->phase == PAIRING_STATE_PHYSICALLY_AUTHORIZED;
}

void pairing_state_mark_rtc_verified(pairing_state_t *state)
{
    if (state && state->phase == PAIRING_STATE_STAGING) {
        state->rtc_verified = true;
    }
}

bool pairing_state_can_commit(pairing_state_t *state,
                              int64_t now_us,
                              bool busy,
                              bool record_complete)
{
    return pairing_state_staging_open(state, now_us, busy) &&
           state->rtc_verified && record_complete;
}

void pairing_state_close(pairing_state_t *state)
{
    if (!state) {
        return;
    }
    state->phase = PAIRING_STATE_CLOSED;
    state->deadline_us = 0;
    state->rtc_verified = false;
}
