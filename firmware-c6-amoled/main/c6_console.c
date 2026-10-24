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
#include "c6_power.h"
#include "c6_ui.h"
#include "c6_voice.h"
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
    else if (!strcmp(cmd, "shot")) c6_ui_screenshot();
    else if (!strcmp(cmd, "settings")) c6_ui_show_settings();
    else if (!strcmp(cmd, "status") || !strcmp(cmd, "talk?")) {
        c6_voice_log_status();
        net_wifi_log_status();
    } else if (!strncmp(cmd, "volume ", 7)) {
        c6_audio_set_volume(atoi(cmd + 7));
        ESP_LOGI(TAG, "volume %d", c6_audio_volume());
    } else if (!strncmp(cmd, "lang ", 5)) {
        const char *l = cmd + 5;
        c6_voice_set_locale(!strcmp(l, "en") ? "en-US" : !strcmp(l, "de") ? "de-DE" : l);
        ESP_LOGI(TAG, "locale %s", c6_voice_locale());
    } else if (!strncmp(cmd, "inject ", 7)) {
        /* KBX inject <bytes>: start a turn whose uplink is the next <bytes>
         * of raw 16 kHz S16LE mono read from this port, sent in real time. */
        long total = atol(cmd + 7);
        /* "KBX inject <bytes> wake": no tap; the clip must start with the
         * wake phrase, which then starts the turn like a spoken one. */
        bool via_wake = strstr(cmd + 7, " wake") != NULL;
        StreamBufferHandle_t sb = total > 0 && total <= 4L * 1024 * 1024 ? c6_voice_inject_begin(via_wake) : NULL;
        if (!sb) {
            ESP_LOGW(TAG, "usage: KBX inject <bytes>");
        } else {
            ESP_LOGI(TAG, "inject: waiting for %ld bytes", total);
            if (!via_wake) c6_voice_tap();
            static uint8_t chunk[256];
            long left = total;
            while (left > 0) {
                /* Up to 30 s until the turn listens, then a 3 s gap ends the clip. */
                int n = usb_serial_jtag_read_bytes(chunk, left < (long)sizeof(chunk) ? (uint32_t)left : sizeof(chunk),
                                                   pdMS_TO_TICKS(left == total ? 30000 : 3000));
                if (n <= 0) break;
                left -= n;
                /* Once the turn stops listening, the rest is read and dropped. */
                size_t queued = 0;
                while (queued < (size_t)n) {
                    c6_voice_state_t st = c6_voice_state();
                    if (st != C6_VOICE_CONNECTING && st != C6_VOICE_LISTENING && !(via_wake && st == C6_VOICE_IDLE)) break;
                    queued += xStreamBufferSend(sb, chunk + queued, (size_t)n - queued, pdMS_TO_TICKS(100));
                }
            }
            ESP_LOGI(TAG, "inject: %ld bytes received", total - left);
            /* Keep the stream until the turn ends so the voice task never sees it vanish. */
            while (c6_voice_state() != C6_VOICE_IDLE && c6_voice_state() != C6_VOICE_ERROR) vTaskDelay(pdMS_TO_TICKS(100));
            c6_voice_inject_end();
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

static void console_task(void *arg)
{
    (void)arg;
    static char line[192];
    size_t used = 0;
    for (;;) {
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
    return xTaskCreate(console_task, "kbx_console", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
