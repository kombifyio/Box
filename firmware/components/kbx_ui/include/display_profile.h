/*
 * kombify box - display profile.
 *
 * Every screen is described by one profile: shape, size class, safe inset
 * and the zone layout (stage = orb, chrome = top chord, caption, status pill,
 * edge ring). UI code reads positions from here and never from board pixels,
 * so a square or wide panel only adds a profile (and, where the layout family
 * differs, a branch on `shape`) instead of new hard-coded offsets.
 *
 * Header is plain C (no LVGL, no ESP-IDF) so touch.c and host tools can use
 * it; the baked rosette for `stage_d` is resolved in aura_orb.c.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { KBX_SHAPE_ROUND, KBX_SHAPE_SQUARE, KBX_SHAPE_WIDE } kbx_shape_t;
/* by short edge: S <= 360, M <= 480, L > 480 */
typedef enum { KBX_SIZE_S, KBX_SIZE_M, KBX_SIZE_L } kbx_size_class_t;

typedef struct {
    const char *id;
    uint16_t w, h;              /* panel pixels after rotation */
    kbx_shape_t shape;          /* round => content inside the inscribed circle */
    kbx_size_class_t size;
    uint8_t safe_inset;         /* bezel/corner inset px */

    /* Zones, screen px. Reference frame for every value: the mockup frames
     * of the same profile (the UI mockups). */
    uint16_t ring_d;            /* edge ring outer diameter; 0 = no ring */
    uint8_t ring_w;             /* edge ring stroke */
    uint16_t stage_cx, stage_cy;/* orb centre */
    uint16_t stage_d;           /* orb diameter (layers are baked for it) */
    uint16_t chrome_cy;         /* centre line of the top chrome */
    uint16_t caption_y;         /* top of the caption block (max 2 lines) */
    uint16_t caption_w;         /* caption width inside the safe area */
    uint16_t pill_cy;           /* centre of the bottom status pill */
    uint16_t touch_r;           /* orb hit radius for LVGL routing */
} kbx_display_profile_t;

/* Profile of the board this image is built for. */
const kbx_display_profile_t *kbx_display_profile(void);

/* Scale a coordinate from the profile's 360 px design grid. For round-360 it
 * is the identity; secondary sheets (pairing, update) are laid out on that
 * grid and keep their proportions on larger panels of the same family. */
static inline int kbx_dp(int v)
{
    const kbx_display_profile_t *p = kbx_display_profile();
    int edge = p->w < p->h ? p->w : p->h;
    return (v * edge + 180) / 360;
}

#ifdef __cplusplus
}
#endif
