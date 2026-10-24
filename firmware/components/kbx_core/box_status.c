/*
 * kombify box - host-driven UI status shared between USB CDC and display.
 */
#include <ctype.h>
#include <string.h>

#include "esp_timer.h"
#include "box_status.h"

static volatile kbx_host_state_t s_state = KBX_HOST_IDLE;
static volatile uint32_t s_updated_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

void box_status_set(kbx_host_state_t state)
{
    s_state = state;
    s_updated_ms = now_ms();
}

kbx_host_state_t box_status_get(uint32_t *age_ms)
{
    uint32_t updated = s_updated_ms;
    if (age_ms) {
        *age_ms = now_ms() - updated;
    }
    return s_state;
}

const char *box_status_name(kbx_host_state_t state)
{
    switch (state) {
    case KBX_HOST_WAKE: return "wake";
    case KBX_HOST_LISTENING: return "listening";
    case KBX_HOST_THINKING: return "thinking";
    case KBX_HOST_SPEAKING: return "speaking";
    case KBX_HOST_DONE: return "done";
    case KBX_HOST_ERROR: return "error";
    case KBX_HOST_IDLE:
    default:
        return "idle";
    }
}

bool box_status_set_from_text(const char *text)
{
    if (!text) {
        return false;
    }

    char cmd[48];
    size_t n = 0;
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    if (starts_with(text, "KBX ") || starts_with(text, "kbx ")) {
        text += 4;
    } else if (starts_with(text, "state=")) {
        text += 6;
    }

    while (*text && !isspace((unsigned char)*text) && n + 1 < sizeof(cmd)) {
        cmd[n++] = (char)tolower((unsigned char)*text++);
    }
    cmd[n] = '\0';
    if (n == 0) {
        return false;
    }

    if (starts_with(cmd, "idle") || starts_with(cmd, "ready")) {
        box_status_set(KBX_HOST_IDLE);
    } else if (starts_with(cmd, "wake")) {
        box_status_set(KBX_HOST_WAKE);
    } else if (starts_with(cmd, "listen") || starts_with(cmd, "capture")) {
        box_status_set(KBX_HOST_LISTENING);
    } else if (starts_with(cmd, "think") || starts_with(cmd, "process")) {
        box_status_set(KBX_HOST_THINKING);
    } else if (starts_with(cmd, "speak") || starts_with(cmd, "tts")) {
        box_status_set(KBX_HOST_SPEAKING);
    } else if (starts_with(cmd, "done")) {
        box_status_set(KBX_HOST_DONE);
    } else if (starts_with(cmd, "error") || starts_with(cmd, "fail")) {
        box_status_set(KBX_HOST_ERROR);
    } else {
        return false;
    }
    return true;
}
