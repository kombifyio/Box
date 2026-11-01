/*
 * kombify box - Aura Orb, UI 2 (rosette centrepiece). See aura_orb.h for the
 * SSOT pointer. State colours are transcribed from tokens.json `assistant`
 * (speechkit.voice_ui.v1); the layer geometry is the CSS stand-in of the
 * UI mockup (`.orb`), which the UI design makes the visual target for the
 * device:
 *
 *   glow    inset -14%  radial lead .42 -> 0 at 68% (farthest corner)
 *   sweep   inset 4%    conic lead -> trail @200deg -> lead, ring mask 57-71%, .95
 *   inner   inset 0     conic from 180deg trail/lead arcs, ring mask 66-71%, .70
 *   halo    inset 10%   2 px lead border, .55, scales with the voice level
 *   core    inset 16%   dark glass #1F2737 -> #0C1119 (38%/30%), 1 px lead rim
 *                       .45, inner bottom glow lead .18, outer glow lead .22
 *   rosette inset 21%   brand mark, full colour, never recoloured
 *   rest    glow .35, rings .30, halo .25, rim #2A3346, no core glows
 *   dim     (muted) rings/glow .15, rosette .40; error rosette .55
 *
 * Rendering (ESP32-S3 budget, §2.6): nothing is computed per frame. Every
 * gradient is baked once at init into PSRAM — state-coloured layers as A8
 * alpha maps shown with image recolour, the fixed dark core as RGB565A8 —
 * with ordered dithering against RGB565 banding. The two-colour conic rings
 * are split exactly into a lead and a trail A8 layer
 *   lead alpha  = a(1-t) / (1 - a t),   trail alpha = a t
 * so "lead over ground, then trail over that" reproduces mix(lead, trail, t)
 * at alpha a for any state colours. Only the halo ring and the rosette scale
 * move with the level; nothing rotates (rotations starved the QSPI LVGL task
 * on this panel, see display.c).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "aura_orb.h"
#include "assets/kombify_rosette.h"
#include "sdkconfig.h"
#include "ui_theme.h"

static const char *TAG = "kbx_aura";

/* ---- tokens.json `shared.aura-<state>-a/-b` -------------------------------- */
typedef struct {
    uint32_t lead;   /* -a: glow / halo / rim / sweep start */
    uint32_t trail;  /* -b: sweep middle / inner arcs        */
    bool alive;      /* states._doc: active:false rests the orb */
} aura_palette_t;

static const aura_palette_t PALETTE[] = {
    [AURA_INACTIVE]   = { 0x9CA3B8, 0x64748B, false },
    [AURA_CONNECTING] = { 0x38BDF8, 0x818CF8, true  },
    [AURA_LISTENING]  = { 0x34D399, 0x38BDF8, true  },
    [AURA_PROCESSING] = { 0xFBBF24, 0xF472B6, true  },
    [AURA_SPEAKING]   = { 0x818CF8, 0xA78BFA, true  },
    [AURA_RECOVERING] = { 0x22D3EE, 0x818CF8, true  },
    [AURA_SETTLING]   = { 0x34D399, 0x818CF8, true  },
    [AURA_ERROR]      = { 0xF87171, 0xFB7185, false },
};

#define REST_RIM        0x2A3346
#define CORE_HI         0x1F2737
#define CORE_LO         0x0C1119

#if CONFIG_KBX_DISPLAY_MONO
#define SCRIM_PCT KBX_REPLY_SCRIM_PCT_MONO
#else
#define SCRIM_PCT KBX_REPLY_SCRIM_PCT
#endif

/* The rosette is baked at the exact size of each orb (MARK_SCALE of the
 * stage, gen_rosette.mjs): 104 px on the classic 180 px orb, 124 px on the
 * 216 px orb of the RLCD watch layout, 167 px on the 288 px orb of the round
 * watch layout. The closest one is used, so no board scales the mark. */
static const lv_image_dsc_t *const ROSETTES[] = {
    &kombify_rosette_104, &kombify_rosette_124, &kombify_rosette_167,
};

/* Layer boxes as fractions of the orb diameter (mockup insets). */
#define GLOW_SCALE      1.28f
#define CORE_SCALE      0.68f
#define HALO_SCALE      0.80f
#define MARK_SCALE      0.58f

/* Layer opacities (0..255). */
#define OPA_GLOW_ALIVE  255
#define OPA_GLOW_REST   89     /* .35 */
#define OPA_GLOW_DIM    38     /* .15 */
#define OPA_RING_ALIVE  255    /* .95/.70 are baked into the maps */
#define OPA_RING_REST   81     /* .30/.95 */
#define OPA_RING_DIM    40     /* .15/.95 */
#define OPA_HALO_ALIVE  140    /* .55 */
#define OPA_HALO_REST   64     /* .25 */
#define OPA_RIM_ALIVE   115    /* .45 */
#define OPA_MARK_DIM    102    /* .40 muted */
#define OPA_MARK_ERROR  140    /* .55 error */

/* Level smoothing (tokens.json assistant): fast-rise attack 0.55/tick,
 * exponential release 5/s. Tick = LEVEL_TICK_MS. */
#define LEVEL_TICK_MS   45
#define LEVEL_ATTACK    0.55f
#define LEVEL_RELEASE   0.7985f  /* exp(-5 * 0.045) */
#define HALO_LEVEL_GAIN 0.10f    /* mockup --lv up to 1.08 */
#define MARK_LEVEL_GAIN 0.07f    /* plan §2.3: rosette 1.00 - 1.07 */
#define LEVEL_STEPS     8.0f

typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *buf;
} baked_t;

static lv_obj_t *s_root;
static lv_obj_t *s_glow, *s_ring_a, *s_ring_b, *s_halo, *s_core, *s_core_glow,
                *s_rim, *s_mark, *s_mark_idle, *s_scrim;
/* Mark activity 0 (idle: greyed, dimmed) .. 255 (active: full colour), eased
 * by an LVGL animation; the scrim opacity animates the same way. */
static int32_t s_act;
static int32_t s_act_target = -1;
static int32_t s_mark_base_opa = LV_OPA_COVER;
static int32_t s_scrim_opa;
static bool s_backdrop;
static baked_t s_glow_l, s_ring_a_l, s_ring_b_l, s_core_l, s_core_glow_l;
static int s_d;
static int s_mark_base;        /* lv scale (256 = 1.0) that maps the asset to MARK_SCALE */
static aura_state_t s_state = AURA_INACTIVE;
static bool s_dim;
static bool s_paused;
static float s_level;
static volatile uint8_t s_level_raw;

/* ---- baking helpers (init only) ------------------------------------------ */

static const uint8_t BAYER4[16] = {
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5,
};

/* Ordered dither offset in [-0.5, 0.5) for pixel (x, y). */
static inline float dither(int x, int y)
{
    return (BAYER4[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f - 0.5f;
}

static uint8_t *psram_alloc(size_t size)
{
    uint8_t *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, size);
}

static bool baked_init(baked_t *l, int size, lv_color_format_t cf)
{
    memset(l, 0, sizeof(*l));
    size_t px = (size_t)size * size;
    size_t bytes = cf == LV_COLOR_FORMAT_RGB565A8 ? px * 3 : px;
    l->buf = psram_alloc(bytes);
    if (!l->buf) return false;
    l->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    l->dsc.header.cf = cf;
    l->dsc.header.w = size;
    l->dsc.header.h = size;
    l->dsc.header.stride = cf == LV_COLOR_FORMAT_RGB565A8 ? size * 2 : size;
    l->dsc.data = l->buf;
    l->dsc.data_size = bytes;
    return true;
}

/* Alpha with an ordered dither of `amp` 8-bit steps, so low-alpha state
 * colours over the dark ground do not band on the RGB565 panel. */
static inline uint8_t alpha_q(float a, int x, int y, float amp)
{
    if (a <= 0.0f) return 0;
    float v = a * 255.0f + (a < 0.999f ? dither(x, y) * amp : 0.0f) + 0.5f;
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)v;
}

/* Anti-aliased coverage of a disc of radius R at distance r (1 px ramp). */
static inline float disc_cov(float r, float R)
{
    float d = R - r + 0.5f;
    return d <= 0.0f ? 0.0f : (d >= 1.0f ? 1.0f : d);
}

static inline float smoothstep01(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* CSS mask radial-gradient(circle, transparent i0, #000 i1, #000 o0,
 * transparent o1), stops in fractions of the farthest-corner radius. */
static inline float ring_mask(float r, float fc, float i0, float i1, float o0, float o1)
{
    return smoothstep01(i0 * fc, i1 * fc, r) * (1.0f - smoothstep01(o0 * fc, o1 * fc, r));
}

/* Gaussian tail 1 - Phi(x / sigma): CSS blur(2*sigma) edge falloff. */
static inline float blur_tail(float x, float sigma)
{
    return 0.5f * erfcf(x / (sigma * 1.41421356f));
}

/* glow (+ the core's outer 24 px glow), lead colour, box GLOW_SCALE * d. */
static void bake_glow(int d)
{
    int g = ((int)(d * GLOW_SCALE)) & ~1;
    if (!baked_init(&s_glow_l, g, LV_COLOR_FORMAT_A8)) return;
    float R = g / 2.0f;
    float fc = R * 1.41421356f;
    float rc = d * CORE_SCALE / 2.0f;
    for (int y = 0; y < g; y++) {
        for (int x = 0; x < g; x++) {
            float dx = x + 0.5f - R, dy = y + 0.5f - R;
            float r = sqrtf(dx * dx + dy * dy);
            float a1 = 0.42f * (1.0f - r / (0.68f * fc));
            if (a1 < 0.0f) a1 = 0.0f;
            float a2 = r > rc ? 0.22f * blur_tail(r - rc, 12.0f) : 0.0f;
            if (a2 > 0.22f) a2 = 0.22f;
            float a = 1.0f - (1.0f - a1) * (1.0f - a2);
            a *= disc_cov(r, R - 0.5f);
            s_glow_l.buf[y * g + x] = alpha_q(a, x, y, 6.0f);
        }
    }
}

/* sweep + inner rings, split into lead and trail A8 maps of size d. */
static void bake_rings(int d)
{
    if (!baked_init(&s_ring_a_l, d, LV_COLOR_FORMAT_A8)) return;
    if (!baked_init(&s_ring_b_l, d, LV_COLOR_FORMAT_A8)) return;
    float R = d / 2.0f;
    float sweep_fc = R * 0.92f * 1.41421356f;   /* inset 4% box */
    float inner_fc = R * 1.41421356f;
    for (int y = 0; y < d; y++) {
        for (int x = 0; x < d; x++) {
            float dx = x + 0.5f - R, dy = y + 0.5f - R;
            float r = sqrtf(dx * dx + dy * dy);
            if (r > R + 0.5f) continue;
            /* CSS conic: 0deg = 12 o'clock, clockwise */
            float deg = atan2f(dx, -dy) * (180.0f / (float)M_PI);
            if (deg < 0.0f) deg += 360.0f;

            /* sweep: lead @0 -> trail @200 -> lead @360, opacity .95 */
            float ms = ring_mask(r, sweep_fc, 0.57f, 0.60f, 0.69f, 0.71f) * 0.95f;
            float ts = deg <= 200.0f ? deg / 200.0f : (360.0f - deg) / 160.0f;

            /* inner: from 180deg; trail 0% -> clear 35% -> lead 62% ->
             * clear 85% -> trail 100%, opacity .70 */
            float mi = ring_mask(r, inner_fc, 0.66f, 0.68f, 0.695f, 0.71f) * 0.70f;
            float p = fmodf(deg + 180.0f, 360.0f) / 360.0f;
            float fi, ti;
            if (p < 0.35f)      { fi = 1.0f - p / 0.35f;               ti = 1.0f; }
            else if (p < 0.62f) { fi = (p - 0.35f) / 0.27f;            ti = 0.0f; }
            else if (p < 0.85f) { fi = 1.0f - (p - 0.62f) / 0.23f;     ti = 0.0f; }
            else                { fi = (p - 0.85f) / 0.15f;            ti = 1.0f; }
            mi *= fi;

            /* rings never overlap (sweep <= 0.71*fc_s < inner >= 0.66*fc_i) */
            float a = ms > mi ? ms : mi;
            float t = ms > mi ? ts : ti;
            a *= disc_cov(r, R - 0.5f);
            float at = a * t;
            float lead = at < 0.999f ? a * (1.0f - t) / (1.0f - at) : 0.0f;
            s_ring_a_l.buf[y * d + x] = alpha_q(lead, x, y, 3.0f);
            s_ring_b_l.buf[y * d + x] = alpha_q(at, x, y, 3.0f);
        }
    }
}

/* Dark glass core (fixed colours, RGB565A8) and its lead inner glow (A8). */
static void bake_core(int d)
{
    int c = ((int)(d * CORE_SCALE)) & ~1;
    if (!baked_init(&s_core_l, c, LV_COLOR_FORMAT_RGB565A8)) return;
    if (!baked_init(&s_core_glow_l, c, LV_COLOR_FORMAT_A8)) return;
    uint16_t *rgb = (uint16_t *)s_core_l.buf;
    uint8_t *alpha = s_core_l.buf + (size_t)c * c * 2;
    float R = c / 2.0f;
    float hx = 0.38f * c, hy = 0.30f * c;                  /* highlight centre */
    float fx = c - hx, fy = c - hy;                        /* farthest corner */
    float span = 0.72f * sqrtf(fx * fx + fy * fy);
    const float hi[3] = { 0x1F, 0x27, 0x37 }, lo[3] = { 0x0C, 0x11, 0x19 };
    for (int y = 0; y < c; y++) {
        for (int x = 0; x < c; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float dx = px - R, dy = py - R;
            float r = sqrtf(dx * dx + dy * dy);
            float cov = disc_cov(r, R - 0.5f);
            float gx = px - hx, gy = py - hy;
            float t = sqrtf(gx * gx + gy * gy) / span;
            if (t > 1.0f) t = 1.0f;
            float dt = dither(x, y);
            int r5 = (int)((hi[0] + (lo[0] - hi[0]) * t) / 255.0f * 31.0f + 0.5f + dt);
            int g6 = (int)((hi[1] + (lo[1] - hi[1]) * t) / 255.0f * 63.0f + 0.5f + dt);
            int b5 = (int)((hi[2] + (lo[2] - hi[2]) * t) / 255.0f * 31.0f + 0.5f + dt);
            r5 = r5 < 0 ? 0 : (r5 > 31 ? 31 : r5);
            g6 = g6 < 0 ? 0 : (g6 > 63 ? 63 : g6);
            b5 = b5 < 0 ? 0 : (b5 > 31 ? 31 : b5);
            rgb[y * c + x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            alpha[y * c + x] = (uint8_t)(cov * 255.0f + 0.5f);

            /* inset 0 -10px 24px lead .18: inner shadow of the disc moved
             * up 10 px, visible along the lower inside edge */
            float sy = py - (R - 10.0f);
            float inside = R - sqrtf(dx * dx + sy * sy);  /* >0 inside shifted disc */
            float a = 0.18f * blur_tail(inside, 12.0f) * cov;
            s_core_glow_l.buf[y * c + x] = alpha_q(a, x, y, 4.0f);
        }
    }
}

/* ---- LVGL objects --------------------------------------------------------- */

static lv_obj_t *layer_image(const baked_t *l, bool recolor)
{
    if (!l->buf) return NULL;
    lv_obj_t *img = lv_image_create(s_root);
    lv_obj_remove_style_all(img);
    lv_image_set_src(img, &l->dsc);
    lv_obj_center(img);
    if (recolor) lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static lv_obj_t *circle(int d)
{
    lv_obj_t *o = lv_obj_create(s_root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_center(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void set_opa(lv_obj_t *o, lv_opa_t opa)
{
    if (o && lv_obj_get_style_opa(o, 0) != opa) lv_obj_set_style_opa(o, opa, 0);
}

/* Level-driven pieces: halo size/opacity, rosette scale. */
static void apply_level(float level)
{
    const aura_palette_t *p = &PALETTE[s_state];
    /* Eight visible steps: each step redraws the orb centre (halo box holds
     * the rosette), so a continuous level would redraw it every 45 ms. */
    level = (float)(int)(level * LEVEL_STEPS + 0.5f) / LEVEL_STEPS;
    bool pulse = p->alive && !s_dim;
    static int s_halo_d = -1;
    static int32_t s_mark_scale = -1;

    int hd = (int)(s_d * HALO_SCALE * (1.0f + (pulse ? level * HALO_LEVEL_GAIN : 0.0f)));
    hd &= ~1;
    if (hd != s_halo_d) {
        lv_obj_set_size(s_halo, hd, hd);
        lv_obj_center(s_halo);
        s_halo_d = hd;
    }
    lv_opa_t hopa = pulse ? (lv_opa_t)(OPA_HALO_ALIVE + level * (255 - OPA_HALO_ALIVE))
                          : OPA_HALO_REST;
    set_opa(s_halo, hopa);

    if (s_mark) {
        int32_t ms = s_mark_base;
        if (pulse && (s_state == AURA_LISTENING || s_state == AURA_SPEAKING)) {
            ms += (int32_t)(s_mark_base * MARK_LEVEL_GAIN * level);
        }
        if (ms != s_mark_scale) {
            lv_image_set_scale(s_mark, (uint32_t)ms);
            if (s_mark_idle) lv_image_set_scale(s_mark_idle, (uint32_t)ms);
            s_mark_scale = ms;
        }
    }
}

static void level_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_paused) return;
    float prev = s_level;
    float target = s_level_raw / 100.0f;
    if (target > s_level) {
        s_level += (target - s_level) * LEVEL_ATTACK;
    } else {
        s_level *= LEVEL_RELEASE;
        if (s_level < target) s_level = target;
    }
    if (s_level < 0.004f) s_level = 0.0f;
    if (s_level == prev && s_level_raw == 0) return;
    apply_level(s_level);
}

/* The idle look of the kombify mark is baked once: saturation and
 * brightness from ui_theme.h applied per pixel (no runtime recolour, which
 * the 1-bit RLCD would only see as a darker blob). The grey copy sits under
 * the colour mark; activity cross-fades between them. */
static lv_image_dsc_t s_idle_dsc;
static uint8_t *s_idle_buf;

static const lv_image_dsc_t *s_rosette = &kombify_rosette_104;   /* this orb's mark */

static const lv_image_dsc_t *mark_idle_src(void)
{
    if (s_idle_buf) return &s_idle_dsc;
    const lv_image_dsc_t *src = s_rosette;
    size_t bytes = src->data_size;
    s_idle_buf = psram_alloc(bytes);
    if (!s_idle_buf) return src;                   /* no PSRAM: keep the colour mark */
    s_idle_dsc = *src;
    s_idle_dsc.data = s_idle_buf;
    const uint8_t *in = src->data;
    for (size_t i = 0; i + 3 < bytes; i += 4) {    /* ARGB8888 in memory: B G R A */
        int b = in[i], g = in[i + 1], r = in[i + 2];
        int y = (r * 77 + g * 150 + b * 29) >> 8;
        int sat = KBX_IDLE_MARK_SAT_PCT, br = KBX_IDLE_MARK_BRIGHT_PCT;
        s_idle_buf[i]     = (uint8_t)(((y * (100 - sat) + b * sat) / 100) * br / 100);
        s_idle_buf[i + 1] = (uint8_t)(((y * (100 - sat) + g * sat) / 100) * br / 100);
        s_idle_buf[i + 2] = (uint8_t)(((y * (100 - sat) + r * sat) / 100) * br / 100);
        s_idle_buf[i + 3] = in[i + 3];
    }
    return &s_idle_dsc;
}

/* Mark layers for an activity 0 (idle) .. 255 (active). */
static void mark_appear(int32_t act, int32_t base_opa)
{
    if (!s_mark_idle) {   /* no PSRAM for the grey copy: dim the colour mark instead */
        int32_t idle = 255 * KBX_IDLE_MARK_BRIGHT_PCT / 100;
        if (idle > base_opa) idle = base_opa;
        set_opa(s_mark, (lv_opa_t)(idle + (base_opa - idle) * act / 255));
        return;
    }
    int32_t idle_opa = s_dim ? OPA_MARK_DIM : LV_OPA_COVER;
    set_opa(s_mark_idle, (lv_opa_t)(idle_opa * (255 - act) / 255));
    set_opa(s_mark, (lv_opa_t)(base_opa * act / 255));
}

static void act_exec_cb(void *var, int32_t v)
{
    (void)var;
    s_act = v;
    mark_appear(s_act, s_mark_base_opa);
}

static void scrim_exec_cb(void *var, int32_t v)
{
    (void)var;
    s_scrim_opa = v;
    if (s_scrim) lv_obj_set_style_bg_opa(s_scrim, (lv_opa_t)v, 0);
}

static void ease_to(void *var, lv_anim_exec_xcb_t cb, int32_t from, int32_t to)
{
    lv_anim_delete(var, cb);
    if (from == to) { cb(var, to); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, KBX_STATE_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void apply_palette(void)
{
    const aura_palette_t *p = &PALETTE[s_state];
    lv_color_t lead = lv_color_hex(p->lead);
    lv_color_t trail = lv_color_hex(p->trail);
    bool alive = p->alive && !s_dim;

    lv_obj_set_style_image_recolor(s_glow, lead, 0);
    lv_obj_set_style_image_recolor(s_ring_a, lead, 0);
    lv_obj_set_style_image_recolor(s_ring_b, trail, 0);
    lv_obj_set_style_image_recolor(s_core_glow, lead, 0);
    lv_obj_set_style_border_color(s_halo, lead, 0);

    set_opa(s_glow, s_dim ? OPA_GLOW_DIM : (alive ? OPA_GLOW_ALIVE : OPA_GLOW_REST));
    lv_opa_t ring = s_dim ? OPA_RING_DIM : (alive ? OPA_RING_ALIVE : OPA_RING_REST);
    set_opa(s_ring_a, ring);
    set_opa(s_ring_b, ring);
    if (alive) lv_obj_remove_flag(s_core_glow, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_core_glow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(s_rim, alive ? lead : lv_color_hex(REST_RIM), 0);
    lv_obj_set_style_border_opa(s_rim, alive ? OPA_RIM_ALIVE : LV_OPA_COVER, 0);

    /* Idle (and muted) rests the mark greyed and dimmed; every other state
     * takes full colour (ui_theme.h KBX_IDLE_MARK_*), eased over STATE_FADE_MS. */
    s_mark_base_opa = s_dim ? OPA_MARK_DIM : (s_state == AURA_ERROR ? OPA_MARK_ERROR : LV_OPA_COVER);
    int32_t act = (s_state != AURA_INACTIVE && !s_dim) ? 255 : 0;
    if (act != s_act_target) {
        s_act_target = act;
        ease_to(&s_act, act_exec_cb, s_act, act);
    } else {
        mark_appear(s_act, s_mark_base_opa);
    }
    apply_level(s_level);
}

/* ---- public API ----------------------------------------------------------- */

lv_color_t aura_orb_lead_color(aura_state_t state)
{
    if ((unsigned)state >= sizeof(PALETTE) / sizeof(PALETTE[0])) state = AURA_INACTIVE;
    return lv_color_hex(PALETTE[state].lead);
}

esp_err_t aura_orb_create(lv_obj_t *parent, const kbx_display_profile_t *prof)
{
    int d = prof->stage_d & ~1;
    s_d = d;
    int box = ((int)(d * GLOW_SCALE)) & ~1;

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, box, box);
    lv_obj_set_pos(s_root, prof->stage_cx - box / 2, prof->stage_cy - box / 2);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    bake_glow(d);
    bake_rings(d);
    bake_core(d);

    s_glow = layer_image(&s_glow_l, true);
    s_ring_a = layer_image(&s_ring_a_l, true);
    s_ring_b = layer_image(&s_ring_b_l, true);
    s_halo = circle((int)(d * HALO_SCALE) & ~1);
    lv_obj_set_style_border_width(s_halo, 2, 0);
    s_core = layer_image(&s_core_l, false);
    s_core_glow = layer_image(&s_core_glow_l, true);
    s_rim = circle(s_core_l.dsc.header.w);

    /* Rosette: pre-baked at the profile's exact size (gen_rosette.mjs); any
     * other stage falls back to the nearest asset with AA scaling. */
    int want = (int)(d * MARK_SCALE + 0.5f);
    for (size_t i = 0; i < sizeof(ROSETTES) / sizeof(ROSETTES[0]); i++) {
        if (abs((int)ROSETTES[i]->header.w - want) < abs((int)s_rosette->header.w - want)) s_rosette = ROSETTES[i];
    }
    const lv_image_dsc_t *mark = s_rosette;
    s_mark_base = (int)(256.0f * want / (float)mark->header.w + 0.5f);
    if (abs(s_mark_base - 256) <= 2) s_mark_base = 256;
    const lv_image_dsc_t *idle_src = mark_idle_src();
    if (idle_src != mark) {
        s_mark_idle = lv_image_create(s_root);
        lv_obj_remove_style_all(s_mark_idle);
        lv_image_set_src(s_mark_idle, idle_src);
        lv_image_set_antialias(s_mark_idle, true);
        lv_obj_center(s_mark_idle);
        lv_image_set_scale(s_mark_idle, (uint32_t)s_mark_base);
        lv_obj_remove_flag(s_mark_idle, LV_OBJ_FLAG_CLICKABLE);
    }
    s_mark = lv_image_create(s_root);
    lv_obj_remove_style_all(s_mark);
    lv_image_set_src(s_mark, mark);
    lv_image_set_antialias(s_mark, true);
    lv_obj_center(s_mark);
    lv_image_set_scale(s_mark, (uint32_t)s_mark_base);
    lv_obj_remove_flag(s_mark, LV_OBJ_FLAG_CLICKABLE);

    /* Reply overlay scrim above every orb layer (the ground colour, so it
     * only dims what is behind it). */
    s_scrim = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, box, box);
    lv_obj_center(s_scrim);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(KBX_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (!s_glow || !s_ring_a || !s_ring_b || !s_core || !s_core_glow) {
        ESP_LOGE(TAG, "layer bake failed (PSRAM?)");
        return ESP_ERR_NO_MEM;
    }

    lv_timer_create(level_timer_cb, LEVEL_TICK_MS, NULL);
    apply_palette();
    ESP_LOGI(TAG, "aura orb up (d=%d, rosette %d px, layers ~%u KB PSRAM)", d, want,
             (unsigned)((s_glow_l.dsc.data_size + s_ring_a_l.dsc.data_size +
                         s_ring_b_l.dsc.data_size + s_core_l.dsc.data_size +
                         s_core_glow_l.dsc.data_size) / 1024));
    return ESP_OK;
}

void aura_orb_set_state(aura_state_t state)
{
    if ((unsigned)state >= sizeof(PALETTE) / sizeof(PALETTE[0])) state = AURA_INACTIVE;
    if (state == s_state) return;
    s_state = state;
    apply_palette();
}

void aura_orb_set_backdrop(bool on)
{
    if (!s_scrim || on == s_backdrop) return;
    s_backdrop = on;
    ease_to(&s_scrim_opa, scrim_exec_cb, s_scrim_opa, on ? 255 * SCRIM_PCT / 100 : 0);
}

lv_obj_t *aura_orb_idle_mark_create(lv_obj_t *parent, int d)
{
    const lv_image_dsc_t *src = mark_idle_src();
    /* A d x d box, the scaled image centred in it exactly like the orb's mark
     * (the image keeps its source size inside the box, scaled about its centre). */
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, d, d);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *img = lv_image_create(box);
    lv_obj_remove_style_all(img);
    lv_image_set_src(img, src);
    lv_image_set_antialias(img, true);
    lv_obj_center(img);
    lv_image_set_scale(img, (uint32_t)(256 * d / src->header.w));
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

void aura_orb_set_dim(bool dim)
{
    if (dim == s_dim) return;
    s_dim = dim;
    apply_palette();
}

void aura_orb_set_level(uint8_t level_0_100)
{
    s_level_raw = level_0_100 > 100 ? 100 : level_0_100;
}

void aura_orb_set_paused(bool paused)
{
    s_paused = paused;
}

bool aura_orb_paused(void) { return s_paused; }

/* No continuous orb animation runs on this panel (see header comment). */
bool aura_orb_anims_running(void) { return lv_anim_count_running() != 0; }

aura_state_t aura_orb_state(void) { return s_state; }

void aura_orb_set_hidden(bool hidden)
{
    if (!s_root || lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
