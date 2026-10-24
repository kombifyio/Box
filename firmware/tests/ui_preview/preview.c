/*
 * Host preview of the Box UI 2 (parity check against the device rendering).
 *
 * Compiles the real UI sources (ui_scene.c, ui_overlays.c, ui_style.c,
 * aura_orb.c, i18n, fonts, rosette) against LVGL with the device's render
 * settings, drives them through the mockup frames with stubbed device state
 * and writes one 360x360 PPM per frame (RGB565 -> RGB888, round mask).
 *
 *   tests/ui_preview/build.sh <lvgl-src> <out-dir>     (see build.sh)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "audio.h"
#include "box_status.h"
#include "cloud_auth.h"
#include "display_profile.h"
#include "fw_ota.h"
#include "i18n.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "touch.h"
#include "ui.h"
#include "device_config.h"
#include "wakeword.h"

/* ------------------------------------------------------------ fake state */

static int64_t g_now_us = 1000000;
static uint32_t g_mic, g_spk, g_volume = 64;
static bool g_muted, g_speaking, g_ready = true, g_wifi = true, g_conversation;
static kbx_host_state_t g_host = KBX_HOST_IDLE;
static int64_t g_host_since;
static cloud_auth_status_t g_cloud;
static fw_ota_update_status_t g_ota;
static bool g_has_error;
static kbx_voice_error_t g_error;
static int64_t g_error_at;
static char g_caption[256];
static net_ws_caption_kind_t g_caption_kind;
static bool g_caption_final;
static uint32_t g_caption_seq;

int64_t esp_timer_get_time(void) { return g_now_us; }

bool audio_is_speaking(void) { return g_speaking; }
bool audio_is_muted(void) { return g_muted; }
uint32_t audio_mic_level(void) { return g_mic; }
uint32_t audio_speaker_level(void) { return g_spk; }
uint32_t audio_volume(void) { return g_volume; }
bool audio_speaker_ready(void) { return true; }
uint32_t audio_set_volume(uint32_t v) { g_volume = v; return v; }
void audio_set_local_mute(bool m) { g_muted = m; }
bool audio_local_mute(void) { return g_muted; }

kbx_host_state_t box_status_get(uint32_t *age_ms)
{
    if (age_ms) *age_ms = (uint32_t)((g_now_us - g_host_since) / 1000);
    return g_host;
}

void cloud_auth_get_status(cloud_auth_status_t *out) { *out = g_cloud; }
esp_err_t cloud_auth_start_pairing(void) { return ESP_OK; }
esp_err_t cloud_auth_forget(void) { return ESP_OK; }

void fw_ota_get_update_status(fw_ota_update_status_t *out) { *out = g_ota; }
esp_err_t fw_ota_check_update(void) { return ESP_OK; }
esp_err_t fw_ota_install_update(void) { return ESP_OK; }

bool net_wifi_is_connected(char *ip, size_t n) { (void)ip; (void)n; return g_wifi; }
bool net_wifi_is_enabled(void) { return g_wifi; }
esp_err_t net_wifi_set_enabled(bool e) { g_wifi = e; return ESP_OK; }

bool net_ws_is_connected(void) { return g_ready; }
bool net_ws_ready_for_talk(void) { return g_ready; }
bool net_ws_talk_active(void) { return g_host == KBX_HOST_LISTENING; }
bool net_ws_talk_connecting(void) { return false; }
bool net_ws_tool_activity(char *name, size_t capacity)
{
    if (name && capacity) name[0] = '\0';
    return false;
}
bool net_ws_conversation_start(void) { return true; }
bool net_ws_conversation_active(void) { return g_conversation; }
void net_ws_talk_cancel(void) {}
bool net_ws_tls_busy(void) { return false; }
esp_err_t net_ws_use_cloud(void) { return ESP_OK; }
esp_err_t net_ws_set_agent_enabled(bool e) { (void)e; return ESP_OK; }
bool net_ws_last_error(kbx_voice_error_t *out, uint32_t *age_ms)
{
    *out = g_error;
    if (age_ms) *age_ms = (uint32_t)((g_now_us - g_error_at) / 1000);
    return g_has_error;
}
bool net_ws_caption(char *out, size_t cap, net_ws_caption_kind_t *kind, bool *final, uint32_t *seq)
{
    snprintf(out, cap, "%s", g_caption);
    if (kind) *kind = g_caption_kind;
    if (final) *final = g_caption_final;
    if (seq) *seq = g_caption_seq;
    return g_caption[0] != '\0';
}

void touch_get_point(int *x, int *y, bool *p) { *x = 0; *y = 0; *p = false; }
bool touch_point_on_orb(int x, int y) { (void)x; (void)y; return false; }
void touch_set_locked(bool l) { (void)l; }

static bool g_wake;
void wakeword_get_status(wakeword_status_t *st) { memset(st, 0, sizeof(*st)); st->enabled = g_wake; st->ready = g_wake; }
esp_err_t wakeword_set_enabled(bool e) { g_wake = e; return ESP_OK; }
int wakeword_phrase_count(void) { return 4; }
const char *wakeword_phrase_text(int i) { static const char *t[] = { "Hi ESP", "Jarvis", "Computer", "Mycroft" }; return t[i & 3]; }
int wakeword_get_phrase(void) { return 0; }
esp_err_t wakeword_set_phrase(int i) { (void)i; return ESP_OK; }
void device_config_note_local_change(void) {}
void device_config_reset(void) {}

/* ---------------------------------------------------------------- render */

#define W 360
#define H 360
static uint16_t s_fb[W * H];
static uint16_t s_buf1[W * 40], s_buf2[W * 40];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) {
        memcpy(&s_fb[y * W + a->x1], src, (size_t)w * 2);
        src += w;
    }
    lv_display_flush_ready(d);
}

static uint32_t tick_cb(void) { return (uint32_t)(g_now_us / 1000); }

static void run_ms(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        g_now_us += 5000;
        lv_timer_handler();
    }
}

static void dump(const char *dir, const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t c = s_fb[y * W + x];
            unsigned char rgb[3] = {
                (unsigned char)(((c >> 11) & 31) * 255 / 31),
                (unsigned char)(((c >> 5) & 63) * 255 / 63),
                (unsigned char)((c & 31) * 255 / 31),
            };
            float dx = x + 0.5f - W / 2.0f, dy = y + 0.5f - H / 2.0f;
            if (kbx_display_profile()->shape == KBX_SHAPE_ROUND && dx * dx + dy * dy > (W / 2.0f) * (W / 2.0f)) {
                rgb[0] = 0x07; rgb[1] = 0x09; rgb[2] = 0x0D;
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    fprintf(stderr, "frame %-22s lvgl pool used %u%% (max %u B, free %u B)\n", name,
            (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.free_size);
}

static void reset_state(void)
{
    ui_notify_settings(false);
    g_mic = g_spk = 0;
    g_muted = g_speaking = g_conversation = false;
    g_ready = g_wifi = true;
    g_host = KBX_HOST_IDLE;
    g_host_since = g_now_us - 60000000;
    g_has_error = false;
    memset(&g_cloud, 0, sizeof(g_cloud));
    g_cloud.paired = true;
    memset(&g_ota, 0, sizeof(g_ota));
    g_caption[0] = '\0';
    run_ms(1200);
}

static void host(kbx_host_state_t s) { g_host = s; g_host_since = g_now_us; }

static void caption(net_ws_caption_kind_t k, bool final, const char *t)
{
    snprintf(g_caption, sizeof(g_caption), "%s", t);
    g_caption_kind = k;
    g_caption_final = final;
    g_caption_seq++;
}

static void error(kbx_voice_error_kind_t kind, int http, const char *code)
{
    memset(&g_error, 0, sizeof(g_error));
    g_error.kind = kind;
    g_error.http_status = http;
    snprintf(g_error.code, sizeof(g_error.code), "%s", code);
    g_error_at = g_now_us;
    g_has_error = true;
    host(KBX_HOST_ERROR);
}

static void frames(const char *dir, const char *loc)
{
    char n[64];
    i18n_set_locale_tag(loc);
#define SHOT(id) do { snprintf(n, sizeof(n), "%s-%s", loc, id); dump(dir, n); } while (0)

    reset_state(); run_ms(400); SHOT("R-01-idle");

    reset_state(); host(KBX_HOST_LISTENING); g_mic = 60;
    caption(NET_WS_CAPTION_INPUT, false, "Dim the living room to thirty");
    run_ms(700); SHOT("R-03-listening");

    reset_state(); host(KBX_HOST_THINKING);
    caption(NET_WS_CAPTION_INPUT, true, "Dim the living room to thirty percent");
    run_ms(700); SHOT("R-04-thinking");

    reset_state(); host(KBX_HOST_SPEAKING); g_spk = 35; g_speaking = true;
    caption(NET_WS_CAPTION_OUTPUT, false,
            "Done. The living room is at 30 %. I also closed the blinds in the kitchen "
            "and the hallway because the sun is setting.");
    run_ms(700); SHOT("R-05-speaking");

    reset_state(); g_conversation = true; host(KBX_HOST_LISTENING); g_mic = 20;
    run_ms(700); SHOT("R-06-conversation");

    reset_state(); g_muted = true; run_ms(500); SHOT("R-07-muted");

    reset_state(); g_cloud.paired = false; g_cloud.pairing = true;
    snprintf(g_cloud.user_code, sizeof(g_cloud.user_code), "WDJB-MJHT");
    snprintf(g_cloud.verification_uri, sizeof(g_cloud.verification_uri), "https://login.kombify.io/activate");
    snprintf(g_cloud.verification_uri_complete, sizeof(g_cloud.verification_uri_complete),
             "https://login.kombify.io/activate?user_code=WDJB-MJHT");
    g_cloud.expires_in = 900;
    ui_notify_cloud_pairing(); run_ms(400); g_now_us += 200 * 1000000LL; run_ms(200);
    SHOT("R-08-pairing");

    reset_state(); g_cloud.paired = true; ui_notify_cloud_pairing(); run_ms(400);
    SHOT("R-08b-paired");

    reset_state(); g_cloud.paired = false; g_cloud.pairing = false;
    ui_notify_cloud_pairing(); run_ms(400); SHOT("R-08c-pair-retry");

    reset_state(); g_ota.state = FW_OTA_UPDATE_INSTALLING; g_ota.progress_percent = 64;
    snprintf(g_ota.version, sizeof(g_ota.version), "0.78.0");
    ui_notify_updates(); run_ms(400); SHOT("R-10-update");

    reset_state(); g_ota.state = FW_OTA_UPDATE_AVAILABLE;
    snprintf(g_ota.version, sizeof(g_ota.version), "0.78.0");
    ui_notify_updates(); run_ms(400); SHOT("R-10b-update-available");

    reset_state(); error(KBX_VOICE_ERR_SERVICE, 503, "speechkit_unavailable");
    run_ms(500); SHOT("R-11-error");

    reset_state(); error(KBX_VOICE_ERR_DENIED, 0, "voice_consent_required");
    run_ms(500); SHOT("R-11b-error-consent");

    reset_state(); ui_notify_swipe(1); run_ms(300); SHOT("X-volume");

    reset_state(); g_wake = false; ui_notify_settings(true); run_ms(1500); SHOT("X-settings");
    ui_notify_settings(false); run_ms(1500);
    g_wake = true; ui_notify_settings(true); run_ms(1500); SHOT("X-settings-wake-on");
    g_wake = false;
    ui_notify_settings(false); run_ms(1500);
#undef SHOT
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_buf1, s_buf2, sizeof(s_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);

    ui_scene_build(d);
    run_ms(500);

    const char *locs[] = { "en", "de", "zh-Hans", "ar" };
    int only = argc > 2;
    for (size_t i = 0; i < sizeof(locs) / sizeof(locs[0]); i++) {
        if (only && strcmp(argv[2], locs[i])) continue;
        frames(dir, locs[i]);
    }
    return 0;
}
