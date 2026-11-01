/*
 * kombify box C6 - operator commands on the built-in USB-Serial/JTAG port.
 * The syntax matches the S3 Box CDC protocol (docs/cdc-protocol.md) where a
 * command exists on both: KBX wifi, KBX cloud, KBX talk, KBX reboot. Secrets
 * (Wi-Fi password, tokens) are never echoed.
 */
#include "c6_console.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "c6_audio.h"
#include "c6_edge.h"
#include "display_design.h"
#include "c6_power.h"
#include "c6_ui.h"
#include "c6_voice.h"
#include "c6_sd.h"
#include "c6_wake.h"
#include "cloud_auth.h"
#include "net_wifi.h"

static const char *TAG = "kbx";

static void cmd_wifi(char *arg)
{
    if (!strcmp(arg, "?")) {
        net_wifi_log_status();
    } else if (!strcmp(arg, "clear")) {
        ESP_LOGI(TAG, "wifi clear: %s", esp_err_to_name(net_wifi_clear_credentials()));
    } else {
        char *sep = strchr(arg, '|');
        if (!sep) {
            ESP_LOGW(TAG, "usage: KBX wifi <ssid>|<password>");
            return;
        }
        *sep = '\0';
        esp_err_t err = net_wifi_set_credentials(arg, sep + 1);
        ESP_LOGI(TAG, "wifi set ssid=\"%s\" password=%s: %s", arg, sep[1] ? "(set)" : "(open)", esp_err_to_name(err));
        memset(sep + 1, 0, strlen(sep + 1));
    }
}

static void cmd_cloud(const char *arg)
{
    if (!strcmp(arg, "pair")) {
        c6_ui_show_pairing();
    } else if (!strcmp(arg, "forget")) {
        ESP_LOGI(TAG, "cloud forget: %s", esp_err_to_name(cloud_auth_forget()));
    } else {
        cloud_auth_status_t st;
        cloud_auth_get_status(&st);
        ESP_LOGI(TAG, "cloud: paired=%d pairing=%d user_code=%s uri=%s expires_in=%d err=%s", st.paired, st.pairing,
                 st.user_code[0] ? st.user_code : "-", st.verification_uri_complete[0] ? st.verification_uri_complete : "-",
                 st.expires_in, st.error_code[0] ? st.error_code : "-");
    }
}

/* Raw clip bytes that follow a KBX inject command (the console does not
 * parse them as commands). */
static StreamBufferHandle_t s_raw_sb;
static long s_raw_left, s_raw_first;
static unsigned s_raw_stage;
static bool s_raw_wake;

static void dispatch(char *line)
{
    size_t n = strlen(line);
    while (n && isspace((unsigned char)line[n - 1])) line[--n] = '\0';
    if (strncmp(line, "KBX ", 4) != 0) return;
    char *cmd = line + 4;
    if (!strncmp(cmd, "wifi?", 5)) cmd_wifi("?");
    else if (!strncmp(cmd, "wifi ", 5)) cmd_wifi(cmd + 5);
    else if (!strncmp(cmd, "cloud?", 6)) cmd_cloud("?");
    else if (!strncmp(cmd, "cloud ", 6)) cmd_cloud(cmd + 6);
    else if (!strcmp(cmd, "talk")) c6_voice_tap();
    else if (!strncmp(cmd, "frontdoor ", 10)) {
        /* KBX frontdoor route <utterance> | KBX frontdoor mode oneshot|conversation:
         * a front-door receipt for the running session (test console). */
        const char *kind = cmd + 10;
        const char *arg = strchr(kind, ' ');
        char kind_buf[8];
        size_t kind_len = arg ? (size_t)(arg - kind) : strlen(kind);
        while (arg && *arg == ' ') arg++;
        if (kind_len == 0 || kind_len >= sizeof(kind_buf) || !arg) {
            ESP_LOGW(TAG, "usage: KBX frontdoor route <utterance> | KBX frontdoor mode oneshot|conversation");
        } else {
            memcpy(kind_buf, kind, kind_len);
            kind_buf[kind_len] = '\0';
            if (!c6_voice_debug_receipt(kind_buf, arg)) ESP_LOGW(TAG, "frontdoor: not fed");
        }
    }
    else if (!strcmp(cmd, "shot")) c6_ui_screenshot();
    else if (!strncmp(cmd, "design", 6)) {
        /* KBX design [classic|watch]: the display design (account setting
         * display.design); a change restarts the box once it is idle. */
        const char *arg = cmd + 6;
        while (*arg == ' ') arg++;
        kbx_design_t d;
        if (*arg == '\0' || *arg == '?') {
            ESP_LOGI(TAG, "design: running=%s stored=%s", kbx_design_id(kbx_design()), kbx_design_id(kbx_design_stored()));
        } else if (!strcmp(arg, "default")) {
            (void)kbx_design_clear();
            ESP_LOGI(TAG, "design: running=%s stored=%s%s", kbx_design_id(kbx_design()), kbx_design_id(kbx_design_stored()),
                     kbx_design_restart_needed() ? " (restart pending)" : "");
        } else if (kbx_design_from_id(arg, &d)) {
            (void)kbx_design_set(d);
            c6_edge_poke();
            ESP_LOGI(TAG, "design: running=%s stored=%s%s", kbx_design_id(kbx_design()), kbx_design_id(kbx_design_stored()),
                     kbx_design_restart_needed() ? " (restart pending)" : "");
        } else {
            ESP_LOGW(TAG, "design: need KBX design classic|watch|default");
        }
    }
    else if (!strcmp(cmd, "settings")) c6_ui_show_settings();
    else if (!strncmp(cmd, "settings ", 9)) c6_ui_show_settings_at(cmd + 9);
    else if (!strcmp(cmd, "swipe")) c6_ui_swipe_selftest();
    else if (!strcmp(cmd, "status") || !strcmp(cmd, "talk?")) {
        c6_voice_log_status();
        net_wifi_log_status();
    } else if (!strncmp(cmd, "volume ", 7)) {
        c6_audio_set_volume(atoi(cmd + 7));
        ESP_LOGI(TAG, "volume %d", c6_audio_volume());
    } else if (!strncmp(cmd, "followups ", 10)) {
        esp_err_t err = c6_voice_set_max_followups((unsigned)atoi(cmd + 10));
        ESP_LOGI(TAG, "assist max follow-ups %u (%s)", c6_voice_max_followups(), esp_err_to_name(err));
    } else if (!strcmp(cmd, "followups?")) {
        ESP_LOGI(TAG, "assist max follow-ups %u", c6_voice_max_followups());
    } else if (!strcmp(cmd, "sync?")) {
        c6_edge_log_status();
    } else if (!strcmp(cmd, "sync now")) {
        c6_edge_poke();
        ESP_LOGI(TAG, "sync: check-in at the next idle upkeep (within 15 s)");
    } else if (!strncmp(cmd, "lang ", 5)) {
        const char *l = cmd + 5;
        c6_voice_set_locale(!strcmp(l, "en") ? "en-US" : !strcmp(l, "de") ? "de-DE" : l);
        ESP_LOGI(TAG, "locale %s", c6_voice_locale());
    } else if (!strncmp(cmd, "inject more ", 12)) {
        /* KBX inject more <bytes>: the clip for the next follow-up turn of
         * the running test; the sender starts it when the log says
         * "follow-up stage". */
        long total = atol(cmd + 12);
        StreamBufferHandle_t sb = c6_voice_inject_stream_active();
        if (total <= 0 || total > 4L * 1024 * 1024 || !sb) {
            ESP_LOGW(TAG, "inject more: no running test");
        } else {
            s_raw_sb = sb;
            s_raw_left = s_raw_first = total;
            s_raw_stage = c6_voice_stage() ? c6_voice_stage() : 1;   /* sent after the "follow-up stage" log line */
            ESP_LOGI(TAG, "inject: waiting for %ld bytes (follow-up clip)", total);
        }
    } else if (!strncmp(cmd, "inject ", 7)) {
        /* KBX inject <bytes>: start a turn whose uplink is the next <bytes>
         * of raw 16 kHz S16LE mono read from this port, sent in real time.
         * The console stays usable between clips (KBX shot during the turn). */
        long total = atol(cmd + 7);
        /* "KBX inject <bytes> wake": no tap; the clip must start with the
         * wake phrase, which then starts the turn like a spoken one. */
        bool via_wake = strstr(cmd + 7, " wake") != NULL;
        StreamBufferHandle_t sb = total > 0 && total <= 4L * 1024 * 1024 ? c6_voice_inject_begin(via_wake) : NULL;
        if (!sb) {
            ESP_LOGW(TAG, "usage: KBX inject <bytes> [wake] | KBX inject more <bytes>");
        } else {
            ESP_LOGI(TAG, "inject: waiting for %ld bytes", total);
            s_raw_sb = sb;
            s_raw_left = s_raw_first = total;
            s_raw_stage = 0;
            s_raw_wake = via_wake;
            if (!via_wake) c6_voice_tap();
        }
    } else if (!strcmp(cmd, "loop?")) {
        int quiet = 0, tone = 0;
        c6_wake_pause();
        esp_err_t err = c6_audio_loopback_test(&quiet, &tone);
        c6_wake_resume();
        ESP_LOGI(TAG, "loopback: %s quiet_rms=%d tone_rms=%d", esp_err_to_name(err), quiet, tone);
    } else if (!strcmp(cmd, "mic?")) {
        /* Two seconds of capture, peak and RMS per 200 ms; nothing is sent. */
        static int16_t pcm[3200];
        c6_wake_pause();
        if (c6_audio_mic_open() != ESP_OK) {
            ESP_LOGW(TAG, "mic busy");
        } else {
            for (int block = 0; block < 10; ++block) {
                c6_audio_mic_read(pcm, 3200);
                int peak = 0;
                double sum = 0;
                for (int i = 0; i < 3200; ++i) {
                    int v = abs(pcm[i]);
                    if (v > peak) peak = v;
                    sum += (double)pcm[i] * pcm[i];
                }
                ESP_LOGI(TAG, "mic block %d peak=%d rms=%d", block, peak, (int)sqrt(sum / 3200));
            }
            c6_audio_mic_close();
        }
        c6_wake_resume();
    } else if (!strcmp(cmd, "wake?")) {
        c6_wake_log_status();
    } else if (!strncmp(cmd, "wake cutoff ", 12) || !strncmp(cmd, "wake cand ", 10)) {
        /* KBX wake cutoff <0.3..0.999|default>: cutoff used alone; KBX wake cand ...: cutoff while the server verifies. */
        bool cand = cmd[5] == 'c' && cmd[6] == 'a';
        const char *arg = cmd + (cand ? 10 : 12);
        float v = !strcmp(arg, "default") ? 0.0f : strtof(arg, NULL);
        bool ok = cand ? c6_wake_set_candidate_cutoff(v) : c6_wake_set_manifest_cutoff(v);
        if (ok) ESP_LOGI(TAG, "wake %s cutoff %s", cand ? "candidate" : "manifest", v > 0 ? arg : "default");
        else ESP_LOGW(TAG, "usage: KBX wake cutoff|cand <0.3-1.1|default>");
    } else if (!strcmp(cmd, "sd?")) {
        c6_sd_log_status();
    } else if (!strcmp(cmd, "sd ls")) {
        c6_sd_log_list();
    } else if (!strcmp(cmd, "sd on") || !strcmp(cmd, "sd off")) {
        esp_err_t e = c6_sd_set_enabled(cmd[4] == 'n');
        ESP_LOGI(TAG, "sd capture %s (%s)", c6_sd_enabled() ? "on" : "off", esp_err_to_name(e));
    } else if (!strncmp(cmd, "wake ", 5)) {
        const char *arg = cmd + 5;
        c6_wake_phrase_t p = C6_WAKE_COUNT;
        if (!strcmp(arg, "off")) p = C6_WAKE_OFF;
        for (int i = 0; i < C6_WAKE_COUNT; ++i) {
            const char *name = c6_wake_phrase_name((c6_wake_phrase_t)i);
            char id[24];
            size_t n = 0;
            for (; name[n] && n + 1 < sizeof(id); ++n) id[n] = name[n] == ' ' ? '_' : (char)tolower((unsigned char)name[n]);
            id[n] = 0;
            if (!strcmp(arg, id)) p = (c6_wake_phrase_t)i;
        }
        if (p == C6_WAKE_COUNT) {
            ESP_LOGW(TAG, "usage: KBX wake off|kombify|hey_kombify|hey_kubi|hey_jarvis");
        } else {
            c6_wake_set_phrase(p);
            ESP_LOGI(TAG, "wake word %s", p == C6_WAKE_OFF ? "off" : c6_wake_phrase_name(p));
        }
    } else if (!strcmp(cmd, "heap")) {
        ESP_LOGI(TAG, "heap free=%u min=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else if (!strcmp(cmd, "off")) {
        c6_power_off();
    } else if (!strcmp(cmd, "reboot")) {
        ESP_LOGI(TAG, "rebooting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        ESP_LOGW(TAG, "unknown command: %s", cmd);
    }
    memset(line, 0, n);
}

static void raw_pump(void)
{
    static uint8_t chunk[256];
    bool first = s_raw_left == s_raw_first;
    /* Up to 30 s (90 s before a follow-up clip) for the first byte, then a 3 s gap ends the clip. */
    int n = usb_serial_jtag_read_bytes(chunk, s_raw_left < (long)sizeof(chunk) ? (uint32_t)s_raw_left : sizeof(chunk),
                                       pdMS_TO_TICKS(first ? (s_raw_stage ? 90000 : 30000) : 3000));
    if (n <= 0) {
        ESP_LOGW(TAG, "inject: clip stalled with %ld bytes missing", s_raw_left);
        s_raw_left = 0;
        return;
    }
    s_raw_left -= n;
    /* A follow-up clip starts once its stage listens. */
    while (first && s_raw_stage && c6_voice_state() != C6_VOICE_IDLE && c6_voice_state() != C6_VOICE_ERROR &&
           (c6_voice_stage() < s_raw_stage || c6_voice_state() != C6_VOICE_LISTENING))
        vTaskDelay(pdMS_TO_TICKS(20));
    /* Once the turn stops listening, the rest is read and dropped. */
    size_t queued = 0;
    while (queued < (size_t)n) {
        c6_voice_state_t st = c6_voice_state();
        if (st != C6_VOICE_CONNECTING && st != C6_VOICE_LISTENING && !(s_raw_wake && st == C6_VOICE_IDLE)) break;
        queued += xStreamBufferSend(s_raw_sb, chunk + queued, (size_t)n - queued, pdMS_TO_TICKS(100));
    }
    if (s_raw_left == 0) {
        ESP_LOGI(TAG, "inject: clip received (%ld bytes)", s_raw_first);
        if (s_raw_wake) {
            /* Wake test: keep the stream until the hit's turn ends. */
            for (int i = 0; i < 30 && c6_voice_state() == C6_VOICE_IDLE; ++i) vTaskDelay(pdMS_TO_TICKS(100));
            while (c6_voice_state() != C6_VOICE_IDLE && c6_voice_state() != C6_VOICE_ERROR) vTaskDelay(pdMS_TO_TICKS(100));
            c6_voice_inject_end();
            s_raw_wake = false;
        }
    }
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[192];
    size_t used = 0;
    for (;;) {
        if (s_raw_left > 0) {
            raw_pump();
            continue;
        }
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r' || ch == '\n') {
            line[used] = '\0';
            if (used) dispatch(line);
            used = 0;
        } else if (used + 1 < sizeof(line)) {
            line[used++] = (char)ch;
        }
    }
}

esp_err_t c6_console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    /* KBX inject streams 32 KB/s; the 256-byte default dropped bytes. */
    cfg.rx_buffer_size = 4096;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) return err;
    usb_serial_jtag_vfs_use_driver();
    return xTaskCreate(console_task, "kbx_console", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
