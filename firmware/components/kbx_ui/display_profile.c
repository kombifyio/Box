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
 *
 * One ring: the Aura Orb's coloured ring is the
 * status ring and fills the whole display; there is no second edge ring.
 * Everything else sits inside it, upright, never between ring and edge.
 * Radii from the centre (180, 180), from aura_orb.c's masks at d = 360:
 *   ring       sweep band r 133..166, thin inner arcs r 168..180
 *   face       the free inner area r < 133 (dark glass core r 122)
 *   mark       104 px (r 52), centred: about 80 px of air to the ring
 *   chrome     time + Wi-Fi (+ battery in watch), the mute or conversation
 *              chip, centre y 104 above the mark (row width <= 190)
 *   caption    info line, error line and reply centred on the face in front
 *              of the scrimmed mark, width 208 (a 208 x 156 block keeps its
 *              corners at r 130)
 *   pill       34 px state pill or hint, centre y 254 below the mark; the
 *              scene caps its width so its ends stay inside r 133
 * Classic and watch share this geometry; the watch design shows the time
 * in the bold 28 px face and adds the battery. One fixed stage avoids a
 * full-orb relayout on a state change (LVGL budget §2.6). */
static const kbx_display_profile_t PROFILE_ROUND_360 = {
    .id = "round-360",
    .w = 360, .h = 360,
    .shape = KBX_SHAPE_ROUND,
    .size = KBX_SIZE_S,
    .safe_inset = 8,
    .ring_d = 352, .ring_w = 6,
    .stage_cx = 180, .stage_cy = 180, .stage_d = 360,
    .mark_d = 104,
    .chrome_cy = 104,
    .caption_y = 156, .caption_w = 208,
    .pill_cy = 254,
    .touch_r = 132,
};

/* Watch: the same face (see above), the time bold. */
static const kbx_display_profile_t PROFILE_ROUND_360_WATCH = {
    .id = "round-360-watch",
    .w = 360, .h = 360,
    .shape = KBX_SHAPE_ROUND,
    .size = KBX_SIZE_S,
    .safe_inset = 8,
    .ring_d = 352, .ring_w = 6,
    .stage_cx = 180, .stage_cy = 180, .stage_d = 360,
    .mark_d = 104,
    .chrome_cy = 104,
    .caption_y = 156, .caption_w = 208,
    .pill_cy = 254,
    .touch_r = 132,
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
