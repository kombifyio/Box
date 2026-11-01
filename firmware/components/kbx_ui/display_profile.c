/*
 * kombify box - display profiles. One image carries the profiles of its board,
 * selected at build time per board (plan §2.7); the display design
 * (display_design.h, an account setting) picks the classic or the watch one at
 * start. Values are transcribed from the mockup frames of the profile, never
 * invented in screen code.
 */
#include "display_profile.h"
#include "display_design.h"
/* The host UI preview (tests/ui_preview) builds without sdkconfig and
 * renders the 1.85C profile. */
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
/* Waveshare ESP32-S3-RLCD-4.2: wide 400x300 reflective, size class S, no
 * touch. Classic: text first: a small orb top left, the
 * previous message beside it (ui_scene.c), the current one large across the
 * panel:
 *   stage    orb 80 px, centre (52, 52)
 *   chat     previous bubble beside the orb, current bubble 112..252
 *            (ui_scene.c), up to 3 lines at 28 px, 368 wide
 *   pill     centre 278 (261..295) */
static const kbx_display_profile_t PROFILE_WIDE_400X300 = {
    .id = "wide-400x300",
    .w = 400, .h = 300,
    .shape = KBX_SHAPE_WIDE,
    .size = KBX_SIZE_S,
    .safe_inset = 8,
    .ring_d = 0, .ring_w = 0,
    .stage_cx = 52, .stage_cy = 52, .stage_d = 80,
    .chrome_cy = 20,
    .caption_y = 150, .caption_w = 368,
    .pill_cy = 278,
    .touch_r = 0,
};

/* Watch: the watch-face layout (the Pixel Watch design
 * language): the Aura Orb is the face, two solid rims hold the secondary
 * information as on a watch bezel:
 *   rims     top 0..48 (date, time, status icons), bottom 252..300 (room
 *            climate, or the voice state pill), 24 px inner corners
 *   stage    orb 216 px, centre (200, 150); it tucks 6 px under each rim
 *   reply    the current sentence in a bubble in front of the dimmed orb,
 *            up to 5 lines at 28 px, 368 wide, between the rims
 *   pill     centre 276 (inside the bottom rim) */
static const kbx_display_profile_t PROFILE_WIDE_400X300_WATCH = {
    .id = "wide-400x300-watch",
    .w = 400, .h = 300,
    .shape = KBX_SHAPE_WIDE,
    .size = KBX_SIZE_S,
    .safe_inset = 8,
    .ring_d = 0, .ring_w = 0,
    .stage_cx = 200, .stage_cy = 150, .stage_d = 216,
    .chrome_cy = 24,
    .caption_y = 130, .caption_w = 368,
    .pill_cy = 276,
    .touch_r = 0,
};

#else
/* Waveshare ESP32-S3-Touch-LCD-1.85(C): round 360 px, size class S.
 * Classic, mockup frames R-1 ... R-11:
 *   edge ring  r=173 stroke 6  -> outer diameter 352
 *   stage      orb 180 px; R-3/4/5 centre y=142, R-1 y=168, R-7 y=170.
 *              One fixed stage (y=146) avoids a full-orb relayout on every
 *              state change (LVGL budget §2.6: never animate layout).
 *   chrome     top chord, chips at y=30..50 -> centre 40
 *   caption    top 246, padding 54 -> width 252 (2 lines at 18 px)
 *   pill       34 px pill at top 300 -> centre 317; 316 keeps the widest
 *              German pill inside the bottom chord */
static const kbx_display_profile_t PROFILE_ROUND_360 = {
    .id = "round-360",
    .w = 360, .h = 360,
    .shape = KBX_SHAPE_ROUND,
    .size = KBX_SIZE_S,
    .safe_inset = 8,
    .ring_d = 352, .ring_w = 6,
    .stage_cx = 180, .stage_cy = 146, .stage_d = 180,
    .chrome_cy = 40,
    .caption_y = 244, .caption_w = 252,
    .pill_cy = 316,
    .touch_r = 112,
};

/* Watch: the watch-face layout (the Pixel Watch design
 * language): the Aura Orb fills the face and the secondary information sits
 * in a wider rim around it, as on a watch.
 *   edge ring  r=173 stroke 6  -> outer diameter 352 (level meter, in the rim)
 *   rim        36 px (r 144..180): time, Wi-Fi and battery on the top chord,
 *              the state pill on the bottom chord
 *   stage      orb 288 px, centre y=172: its sweep ring ends at y 39..305, so
 *              it clears the time above and the pill below; the mark is 167 px
 *              (0.58 of the orb, as aura_orb.c)
 *   chrome     top chord, centre 26
 *   caption    info line centred on the orb (146), width 252
 *   pill       34 px pill, centre 322 (305..339); the widest German pill stays
 *              inside the bottom chord */
static const kbx_display_profile_t PROFILE_ROUND_360_WATCH = {
    .id = "round-360-watch",
    .w = 360, .h = 360,
    .shape = KBX_SHAPE_ROUND,
    .size = KBX_SIZE_S,
    .safe_inset = 8,
    .ring_d = 352, .ring_w = 6,
    .stage_cx = 180, .stage_cy = 172, .stage_d = 288,
    .chrome_cy = 26,
    .caption_y = 146, .caption_w = 252,
    .pill_cy = 322,
    .touch_r = 144,
};

#endif

const kbx_display_profile_t *kbx_display_profile(void)
{
    const bool watch = kbx_design() == KBX_DESIGN_WATCH;
#if CONFIG_KBX_DISPLAY_SHAPE_WIDE
    return watch ? &PROFILE_WIDE_400X300_WATCH : &PROFILE_WIDE_400X300;
#else
    return watch ? &PROFILE_ROUND_360_WATCH : &PROFILE_ROUND_360;
#endif
}
