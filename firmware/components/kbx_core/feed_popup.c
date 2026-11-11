/*
 * kombify box - Feed popup hand-off. Contract: feed_popup.h.
 */
#include "feed_popup.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR feed_popup_t s_popup;   /* PSRAM: internal RAM stays for TLS and wake word */

void feed_popup_post(const char *subject, const char *kind, unsigned count)
{
    if (!subject || !subject[0]) return;
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_popup.subject, subject, sizeof(s_popup.subject));
    strlcpy(s_popup.kind, kind && kind[0] ? kind : "other", sizeof(s_popup.kind));
    s_popup.count = count ? count : 1;
    s_popup.seq++;
    if (!s_popup.seq) s_popup.seq = 1;   /* 0 means "nothing seen" */
    taskEXIT_CRITICAL(&s_lock);
}

bool feed_popup_take(uint32_t *seen, feed_popup_t *out)
{
    if (!seen || !out) return false;
    bool fresh = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_popup.seq && s_popup.seq != *seen) {
        *seen = s_popup.seq;
        fresh = s_popup.subject[0] != '\0';   /* cleared before the UI saw it */
        if (fresh) *out = s_popup;
    }
    taskEXIT_CRITICAL(&s_lock);
    return fresh;
}

void feed_popup_clear(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_popup.subject[0] = '\0';
    s_popup.count = 0;
    taskEXIT_CRITICAL(&s_lock);
}
