/*
 * kombify box - Feed popup hand-off (ai-platform-1vu2).
 *
 * The edge sync client (edge_sync.c) reads the newest unread Feed items of
 * the paired account on its existing poll (GET <device>/feed on the cycle's
 * connection, Connect contract section 9.2) and posts the newest new one
 * here; the UI (ui_overlays.c) takes it and shows a popup card for
 * FEED_POPUP_SHOW_MS or until a tap/key dismisses it. Items that arrive
 * during quiet hours are held and folded into one popup afterwards.
 *
 * Only the subject, the kind (Feed section) and the count travel: never a
 * body, link or secret. Writer: sync task. Reader: LVGL timer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FEED_POPUP_SHOW_MS    30000
#define FEED_POPUP_SUBJECT_MAX 324   /* 80 chars of up to 4 UTF-8 bytes, NUL */

typedef struct {
    uint32_t seq;                          /* increases with every post */
    char subject[FEED_POPUP_SUBJECT_MAX];
    char kind[12];                         /* household homelab paperwork agents system other */
    unsigned count;                        /* new items folded into this popup (>= 1) */
} feed_popup_t;

/* Sync task: a new popup replaces one not shown yet. */
void feed_popup_post(const char *subject, const char *kind, unsigned count);
/* UI: copies the newest popup when its seq differs from *seen and updates
 * *seen; returns false when nothing new is there. */
bool feed_popup_take(uint32_t *seen, feed_popup_t *out);
/* Pairing reset / popups switched off: drops a popup not shown yet. */
void feed_popup_clear(void);

#ifdef __cplusplus
}
#endif
