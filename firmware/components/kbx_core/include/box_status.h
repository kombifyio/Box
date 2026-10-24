/*
 * kombify box - host-driven UI status shared between USB CDC and display.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KBX_HOST_IDLE = 0,
    KBX_HOST_WAKE,
    KBX_HOST_LISTENING,
    KBX_HOST_THINKING,
    KBX_HOST_SPEAKING,
    KBX_HOST_DONE,
    KBX_HOST_ERROR,
} kbx_host_state_t;

void box_status_set(kbx_host_state_t state);
bool box_status_set_from_text(const char *text);
kbx_host_state_t box_status_get(uint32_t *age_ms);
const char *box_status_name(kbx_host_state_t state);

#ifdef __cplusplus
}
#endif
