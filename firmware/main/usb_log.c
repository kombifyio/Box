/*
 * kombify box - mirror ESP_LOG output to the TinyUSB CDC ACM log port.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "audio.h"
#include "cloud_auth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_core_dump.h"
#include "panic_record.h"
#include "tusb.h"
#include "box_status.h"
#include "diag_i2c.h"
#include "display.h"
#include "display_design.h"
#include "fw_ota.h"
#include "i18n.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "touch.h"
#include "aec_stage.h"
#include "battery.h"
#include "stop_word.h"
#include "env_sensor.h"
#include "es7210_min.h"
#include "es8311_min.h"
#include "lan_console.h"
#include "usb_log.h"
#include "ui.h"
#include "device_config.h"
#include "edge_sync.h"
#include "wakeword.h"

#define CDC_LOG_LINE_MAX 320
/* Must fit the longest KBX line. Used to be 128 (Wi-Fi only), but
 * "KBX token <bearer>" overflows that: an Auth0/OIDC JWT is routinely
 * 700-1000+ characters. 2048 leaves headroom; overlong lines are discarded
 * until newline (with a clear token warning, see usb_log_handle_rx_char). */
#define CDC_RX_LINE_MAX 2048

static vprintf_like_t s_prev_vprintf = NULL;
static SemaphoreHandle_t s_log_lock = NULL;
static SemaphoreHandle_t s_cmd_lock = NULL;
static bool s_installed = false;
static char s_rx_line[CDC_RX_LINE_MAX];
static size_t s_rx_len = 0;
static bool s_rx_overflow = false;

static bool usb_log_line_command_is(const char *line, const char *want)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    if ((line[0] == 'K' || line[0] == 'k') &&
        (line[1] == 'B' || line[1] == 'b') &&
        (line[2] == 'X' || line[2] == 'x') &&
        isspace((unsigned char)line[3])) {
        line += 4;
        while (*line && isspace((unsigned char)*line)) {
            line++;
        }
    }
    while (*want && *line) {
        if (tolower((unsigned char)*line) != tolower((unsigned char)*want)) {
            return false;
        }
        line++;
        want++;
    }
    return *want == '\0' && (*line == '\0' || isspace((unsigned char)*line));
}

/* KBX heap? - who holds internal RAM. Task stacks and TCBs are attributed
 * by matching each task's stack start and TCB against the heap blocks; the
 * largest remaining blocks are listed by size (LVGL draw buffers, I2S DMA,
 * TLS and driver buffers are recognisable from their sizes). Diagnostic
 * only: walks the heap under its lock, no allocation. */
#define HEAP_TASKS_MAX 40
#define HEAP_TOP_MAX   16
typedef struct {
    UBaseType_t tasks;
    uint8_t *stack[HEAP_TASKS_MAX];
    void *tcb[HEAP_TASKS_MAX];
    char name[HEAP_TASKS_MAX][configMAX_TASK_NAME_LEN];
    uint32_t stack_size[HEAP_TASKS_MAX];
    uint32_t tcb_size[HEAP_TASKS_MAX];
    void *top_ptr[HEAP_TOP_MAX];
    uint32_t top_size[HEAP_TOP_MAX];
    int top_owner[HEAP_TOP_MAX];
    uint32_t bin_count[4], bin_bytes[4];
} heap_report_t;
static heap_report_t s_heap_report;

static bool heap_report_block(walker_heap_into_t heap, walker_block_info_t block, void *user)
{
    (void)heap;
    heap_report_t *r = (heap_report_t *)user;
    if (!block.used || !block.ptr) return true;
    uint8_t *lo = (uint8_t *)block.ptr, *hi = lo + block.size;
    uint32_t size = (uint32_t)block.size;
    int bin = size < 256 ? 0 : size < 1024 ? 1 : size < 4096 ? 2 : 3;
    r->bin_count[bin]++;
    r->bin_bytes[bin] += size;
    int owner = -1;
    for (UBaseType_t i = 0; i < r->tasks; i++) {
        if (r->stack[i] >= lo && r->stack[i] < hi) { r->stack_size[i] = size; owner = (int)i; }
        else if ((uint8_t *)r->tcb[i] >= lo && (uint8_t *)r->tcb[i] < hi) { r->tcb_size[i] = size; }
    }
    if (owner < 0) {
        int slot = -1;
        for (int k = 0; k < HEAP_TOP_MAX; k++) {
            if (r->top_size[k] < size && (slot < 0 || r->top_size[k] < r->top_size[slot])) slot = k;
        }
        if (slot >= 0) {
            r->top_ptr[slot] = block.ptr;
            r->top_size[slot] = size;
            r->top_owner[slot] = -1;
        }
    }
    return true;
}

static void usb_log_heap_report(void)
{
    /* Every task this image creates (IDF, components, main). The public
     * FreeRTOS API has no task iterator without the trace facility. */
    static const char *const names[] = {
        "main", "IDLE0", "IDLE1", "ipc0", "ipc1", "esp_timer", "Tmr Svc", "sys_evt",
        "tiT", "wifi", "taskLVGL", "TinyUSB", "usb_mic_task", "usb_spk_task",
        "kbx_mic_broker", "kbx_spk_writer", "kbx_va_spk", "kbx_va_up", "kbx_touch",
        "net_ws", "websocket_task", "kbx_wake", "cloud_pair", "kbx_update",
        "kbx_ota_start", "pair_action", "kbx_diag", "kbx_bt_up", "kbx_tone24",
        "kbx_test_tone", "btController", "nimble_host",
    };
    heap_report_t *r = &s_heap_report;
    memset(r, 0, sizeof(*r));
    UBaseType_t n = 0;
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]) && n < HEAP_TASKS_MAX; k++) {
        TaskHandle_t t = xTaskGetHandle(names[k]);
        if (!t) continue;
        r->tcb[n] = (void *)t;
        r->stack[n] = xTaskGetStackStart(t);
        strlcpy(r->name[n], names[k], sizeof(r->name[n]));
        n++;
    }
    r->tasks = n;

    multi_heap_info_t in, dma, ps;
    heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&dma, MALLOC_CAP_DMA);
    heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM);
    heap_caps_walk(MALLOC_CAP_INTERNAL, heap_report_block, r);

    ESP_LOGI("kbx_heap", "internal free=%u largest=%u min=%u used=%u blocks=%u | dma free=%u largest=%u | psram free=%u largest=%u",
             (unsigned)in.total_free_bytes, (unsigned)in.largest_free_block,
             (unsigned)in.minimum_free_bytes, (unsigned)in.total_allocated_bytes,
             (unsigned)in.allocated_blocks, (unsigned)dma.total_free_bytes,
             (unsigned)dma.largest_free_block, (unsigned)ps.total_free_bytes,
             (unsigned)ps.largest_free_block);
    ESP_LOGI("kbx_heap", "internal used by size: <256B %u/%uB  <1K %u/%uB  <4K %u/%uB  >=4K %u/%uB",
             (unsigned)r->bin_count[0], (unsigned)r->bin_bytes[0],
             (unsigned)r->bin_count[1], (unsigned)r->bin_bytes[1],
             (unsigned)r->bin_count[2], (unsigned)r->bin_bytes[2],
             (unsigned)r->bin_count[3], (unsigned)r->bin_bytes[3]);
    uint32_t stacks = 0, tcbs = 0, nstacks = 0;
    for (UBaseType_t i = 0; i < r->tasks; i++) {
        tcbs += r->tcb_size[i];
        if (r->stack_size[i]) { stacks += r->stack_size[i]; nstacks++; }
    }
    ESP_LOGI("kbx_heap", "tasks=%u: %u stacks in internal heap = %u B, TCBs = %u B (others: PSRAM or static)",
             (unsigned)r->tasks, (unsigned)nstacks, (unsigned)stacks, (unsigned)tcbs);
    for (UBaseType_t i = 0; i < r->tasks; i++) {
        if (!r->stack_size[i]) continue;
        ESP_LOGI("kbx_heap", "  stack %-16s %6u B (free %u)", r->name[i], (unsigned)r->stack_size[i],
                 (unsigned)uxTaskGetStackHighWaterMark((TaskHandle_t)r->tcb[i]));
    }
    for (int pass = 0; pass < HEAP_TOP_MAX; pass++) {
        int best = -1;
        for (int k = 0; k < HEAP_TOP_MAX; k++) {
            if (r->top_size[k] && (best < 0 || r->top_size[k] > r->top_size[best])) best = k;
        }
        if (best < 0) break;
        ESP_LOGI("kbx_heap", "  block %6u B @%p%s", (unsigned)r->top_size[best], r->top_ptr[best],
                 esp_ptr_dma_capable(r->top_ptr[best]) ? " dma" : "");
        r->top_size[best] = 0;
    }
}

/* KBX coredump? | coredump clear - the last panic, saved to the coredump
 * partition by the panic handler (the console is USB CDC, so a panic
 * cannot be printed live). Decode the PCs with addr2line on the ELF whose
 * sha matches. */
static void usb_log_coredump_report(void)
{
    panic_record_log();
    esp_err_t err = esp_core_dump_image_check();
    if (err != ESP_OK) {
        ESP_LOGI("kbx_core", "coredump: none (%s)", esp_err_to_name(err));
        return;
    }
    size_t addr = 0, size = 0;
    (void)esp_core_dump_image_get(&addr, &size);
    static esp_core_dump_summary_t sum;
    memset(&sum, 0, sizeof(sum));
    err = esp_core_dump_get_summary(&sum);
    if (err != ESP_OK) {
        ESP_LOGW("kbx_core", "coredump: %u B at 0x%x, summary failed (%s)",
                 (unsigned)size, (unsigned)addr, esp_err_to_name(err));
        return;
    }
    char sha[17];
    for (int i = 0; i < 8; i++) snprintf(sha + 2 * i, 3, "%02x", sum.app_elf_sha256[i]);
    ESP_LOGI("kbx_core", "coredump: %u B at 0x%x elf=%s task=%s tcb=0x%08lx pc=0x%08lx "
             "exccause=%lu excvaddr=0x%08lx",
             (unsigned)size, (unsigned)addr, sha, sum.exc_task, (unsigned long)sum.exc_tcb,
             (unsigned long)sum.exc_pc, (unsigned long)sum.ex_info.exc_cause,
             (unsigned long)sum.ex_info.exc_vaddr);
    char line[200];
    size_t n = 0;
    line[0] = 0;
    for (uint32_t i = 0; i < sum.exc_bt_info.depth && i < 16 && n < sizeof(line) - 12; i++) {
        n += (size_t)snprintf(line + n, sizeof(line) - n, " 0x%08lx", (unsigned long)sum.exc_bt_info.bt[i]);
    }
    ESP_LOGI("kbx_core", "coredump: backtrace%s (depth %lu):%s",
             sum.exc_bt_info.corrupted ? " CORRUPTED" : "",
             (unsigned long)sum.exc_bt_info.depth, line);
    n = 0;
    line[0] = 0;
    for (int i = 0; i < 16 && n < sizeof(line) - 12; i++) {
        n += (size_t)snprintf(line + n, sizeof(line) - n, " %08lx", (unsigned long)sum.ex_info.exc_a[i]);
    }
    ESP_LOGI("kbx_core", "coredump: a0-a15:%s", line);
}

/* KBX config? | config apply <speechkit.box_settings.v1 JSON>
 * Account device settings: the same apply path the cloud sync uses, so a
 * host (bench, Techstack node) can prove the contract on the device. */
static bool usb_log_handle_config_command(const char *line)
{
    if (usb_log_line_command_is(line, "config?")) {
        device_config_log_status();
    } else if (usb_log_line_command_is(line, "config apply")) {
        const char *p = strchr(line, '{');
        const char *reason = NULL;
        device_config_outcome_t outcome = device_config_apply_json(p, p ? strlen(p) : 0, &reason);
        ESP_LOGI("kbx_cfg", "config apply: %s%s%s", device_config_outcome_name(outcome),
                 reason ? " " : "", reason ? reason : "");
    } else {
        return false;
    }
    return true;
}

/* KBX wake on|off|? | wake test on|off | wake thr <0.50-0.999|0> | wake reset
 * Local wake word (ESP-SR or microWakeWord phrase) and its on-device
 * qualification counters. */
static bool usb_log_handle_wake_command(const char *line)
{
    if (usb_log_line_command_is(line, "wake?")) {
        wakeword_log_status();
    } else if (usb_log_line_command_is(line, "wake on")) {
        if (wakeword_set_enabled(true) == ESP_OK) device_config_note_local_change();
    } else if (usb_log_line_command_is(line, "wake off")) {
        if (wakeword_set_enabled(false) == ESP_OK) device_config_note_local_change();
    } else if (usb_log_line_command_is(line, "wake test on")) {
        wakeword_set_test_mode(true);
    } else if (usb_log_line_command_is(line, "wake test off")) {
        wakeword_set_test_mode(false);
    } else if (usb_log_line_command_is(line, "wake reset")) {
        wakeword_reset_counters();
    } else if (usb_log_line_command_is(line, "wake phrase")) {
        /* KBX wake phrase <hiesp|jarvis|computer|mycroft|kombify|hey_kombify|
         * hey_kubi|hey_jarvis> | ? */
        const char *p = strstr(line, "phrase");
        char id[16] = "";
        int n = p ? sscanf(p + 6, "%15s", id) : 0;
        if (n < 1 || strcmp(id, "?") == 0) wakeword_log_phrases();
        else if (wakeword_set_phrase_id(id) != ESP_OK) {
            ESP_LOGW("kbx_wake", "unknown phrase \"%s\"", id);
            wakeword_log_phrases();
        } else {
            device_config_note_local_change();
        }
    } else if (usb_log_line_command_is(line, "wake opt")) {
        /* KBX wake opt <istack|noext|noarm|mode90|copy|nofloor|core0> [on|off]
         * | clear | ? : crash-isolation variants (dev11). */
        const char *p = strstr(line, "opt");
        char name[16] = "", val[8] = "";
        int n = p ? sscanf(p + 3, "%15s %7s", name, val) : 0;
        if (n < 1 || strcmp(name, "?") == 0) wakeword_log_opts();
        else (void)wakeword_set_opt(name, strcasecmp(val, "off") != 0);
    } else if (usb_log_line_command_is(line, "sound cues?")) {
        ESP_LOGI("kbx_audio", "sound cues %s, style %s (styles: chime pop glass bell)",
                 audio_cues_enabled() ? "on" : "off", audio_cue_style_id(audio_cue_style()));
    } else if (usb_log_line_command_is(line, "sound cues on")) {
        audio_cues_set_enabled(true);
        device_config_note_local_change();
    } else if (usb_log_line_command_is(line, "sound cues off")) {
        audio_cues_set_enabled(false);
        device_config_note_local_change();
    } else if (usb_log_line_command_is(line, "sound cues style")) {
        /* KBX sound cues style <chime|pop|glass|bell>: sets it and plays a preview. */
        const char *p = strstr(line, "style");
        char id[12] = "";
        if (p && sscanf(p + 5, "%11s", id) == 1 && audio_cue_set_style_id(id) == ESP_OK) {
            device_config_note_local_change();
        } else {
            ESP_LOGW("kbx_audio", "need KBX sound cues style <chime|pop|glass|bell>");
        }
    } else if (usb_log_line_command_is(line, "assist?")) {
        ESP_LOGI("net_ws", "assist: max follow-ups %u (KBX assist followups <0-3>)",
                 net_ws_max_followups());
    } else if (usb_log_line_command_is(line, "assist followups")) {
        /* KBX assist followups <0-3>: follow-up stages per tap or wake hit. */
        const char *p = strstr(line, "followups");
        char *end = NULL;
        long v = p ? strtol(p + 9, &end, 10) : -1;
        if (!p || end == p + 9 || net_ws_set_max_followups(v < 0 ? 99U : (unsigned)v) != ESP_OK) {
            ESP_LOGW("net_ws", "need KBX assist followups <0-3>");
        } else {
            device_config_note_local_change();
        }
    } else if (usb_log_line_command_is(line, "wake thr")) {
        const char *p = strstr(line, "thr");
        float v = p ? strtof(p + 3, NULL) : -1.0f;
        if (wakeword_set_threshold(v) == ESP_OK) {
            ESP_LOGI("kbx_wake", "threshold %s", v > 0.0f ? "set" : "reset to the phrase default");
        } else {
            ESP_LOGW("kbx_wake", "need KBX wake thr <0.50-0.999> (0 = phrase default)");
        }
    } else if (usb_log_line_command_is(line, "wake gain")) {
        /* KBX wake gain <1-16|0>: microWakeWord input gain (0 = default x1). */
        const char *p = strstr(line, "gain");
        char *end = NULL;
        long v = p ? strtol(p + 4, &end, 10) : -1;
        if (!p || end == p + 4 || v < 0 || wakeword_set_mww_gain((unsigned)v) != ESP_OK) {
            ESP_LOGW("kbx_wake", "need KBX wake gain <1-16> (0 = default x1), now x%u", wakeword_mww_gain());
        }
    } else if (usb_log_line_command_is(line, "mic probe")) {
        audio_mic_probe_start();
    } else if (usb_log_line_command_is(line, "mic slot?")) {
        audio_mic_log_slot();
    } else if (usb_log_line_command_is(line, "mic slot")) {
        /* KBX mic slot left|right|mix|auto (persisted). */
        const char *p = strstr(line, "slot");
        char name[8] = "";
        if (!p || sscanf(p + 4, "%7s", name) != 1 || audio_mic_set_slot(name) != ESP_OK) {
            ESP_LOGW("kbx_audio", "need KBX mic slot left|right|mix|auto");
            audio_mic_log_slot();
        }
    } else if (usb_log_line_command_is(line, "voice?")) {
        net_ws_log_voice();
    } else if (usb_log_line_command_is(line, "voice")) {
        /* KBX voice auto|female|male: reply voice gender (assist.voice). */
        const char *p = strstr(line, "voice");
        char id[8] = "";
        if (p && sscanf(p + 5, "%7s", id) == 1 && net_ws_set_voice_id(id) == ESP_OK) {
            device_config_note_local_change();
        } else {
            ESP_LOGW("net_ws", "need KBX voice auto|female|male");
            net_ws_log_voice();
        }
    } else {
        return false;
    }
    return true;
}

/* Log capture for the LAN console: every log line while a capture is open
 * is appended (truncated at the buffer end). One capture at a time. */
static portMUX_TYPE s_capture_mux = portMUX_INITIALIZER_UNLOCKED;
static char *volatile s_capture_buf;
static size_t s_capture_cap, s_capture_len;

static void usb_log_capture_append(const char *format, va_list args)
{
    char line[CDC_LOG_LINE_MAX];
    int len = vsnprintf(line, sizeof(line), format, args);
    if (len <= 0) return;
    size_t n = len < (int)sizeof(line) ? (size_t)len : sizeof(line) - 1;
    taskENTER_CRITICAL(&s_capture_mux);
    if (s_capture_buf && s_capture_len + 1 < s_capture_cap) {
        size_t room = s_capture_cap - 1 - s_capture_len;
        if (n > room) n = room;
        memcpy(s_capture_buf + s_capture_len, line, n);
        s_capture_len += n;
        s_capture_buf[s_capture_len] = '\0';
    }
    taskEXIT_CRITICAL(&s_capture_mux);
}

bool usb_log_capture_begin(char *buf, size_t cap)
{
    if (!buf || cap < 2) return false;
    bool ok = false;
    taskENTER_CRITICAL(&s_capture_mux);
    if (!s_capture_buf) {
        buf[0] = '\0';
        s_capture_cap = cap;
        s_capture_len = 0;
        s_capture_buf = buf;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_capture_mux);
    return ok;
}

size_t usb_log_capture_end(void)
{
    taskENTER_CRITICAL(&s_capture_mux);
    size_t n = s_capture_len;
    s_capture_buf = NULL;
    s_capture_cap = s_capture_len = 0;
    taskEXIT_CRITICAL(&s_capture_mux);
    return n;
}

static int usb_log_vprintf(const char *format, va_list args)
{
    va_list cdc_args;
    va_copy(cdc_args, args);

    int ret = s_prev_vprintf ? s_prev_vprintf(format, args) : vprintf(format, args);

    if (s_capture_buf) {
        va_list cap_args;
        va_copy(cap_args, cdc_args);
        usb_log_capture_append(format, cap_args);
        va_end(cap_args);
    }

    /* CDC mirror only when the host is actually listening. Do not flush
     * per line: TinyUSB + UAC share the USB bus; 1 Hz audio INFO measurably
     * stalled the audio path. */
    if (s_log_lock && tud_mounted() && tud_cdc_connected() &&
        xSemaphoreTake(s_log_lock, 0) == pdTRUE) {
        char line[CDC_LOG_LINE_MAX];
        int len = vsnprintf(line, sizeof(line), format, cdc_args);
        if (len > 0) {
            size_t write_len = (len < (int)sizeof(line)) ? (size_t)len : sizeof(line) - 1;
            uint32_t available = tud_cdc_write_available();
            if (available > 0) {
                if (write_len > available) {
                    write_len = available;
                }
                tud_cdc_write(line, (uint32_t)write_len);
                /* Flush INFO from kbx_ota (host waits on "recv") and all
                 * warnings/errors. Idle audio telemetry no longer logs. */
                if (available - write_len < 128 ||
                    (write_len > 0 && (line[0] == 'E' || line[0] == 'W' ||
                                       (len >= 20 && strstr(line, "kbx_ota:") != NULL)))) {
                    tud_cdc_write_flush();
                }
            }
        }
        xSemaphoreGive(s_log_lock);
    }

    va_end(cdc_args);
    return ret;
}

void usb_log_install(void)
{
    if (s_installed) {
        return;
    }

    s_log_lock = xSemaphoreCreateMutex();
    s_cmd_lock = xSemaphoreCreateMutex();
    if (!s_log_lock || !s_cmd_lock) {
        return;
    }

    s_prev_vprintf = esp_log_set_vprintf(usb_log_vprintf);
    s_installed = true;
}

/* "KBX wifi <ssid>|<pass>": pipe splits SSID and password so both may
 * contain spaces. "KBX wifi?" = status, "KBX wifi clear" clears credentials. */
/* KBX shot: the panel frame over the CDC for UI review (round board; the
 * RLCD frame is 1 bit, use kbx-remote.ps1 -Screenshot there). One task
 * captures the frame, then streams "#SHOT <w> <h> rgb565", one
 * "#SHOT <y> <base64 of the row>" line per row and "#SHOT END", holding the
 * log mirror off so no log line lands inside a row. firmware/tools/
 * s3-screenshot.py reassembles the PNG. */
#define SHOT_W 360
#define SHOT_H 360

static bool shot_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    int stalled = 0;
    while (len) {
        uint32_t room = tud_cdc_write_available();
        if (!room) {
            tud_cdc_write_flush();
            if (++stalled > 500) return false;   /* 5 s without the host reading */
            vTaskDelay(1);
            continue;
        }
        stalled = 0;
        uint32_t n = len < room ? (uint32_t)len : room;
        tud_cdc_write(p, n);
        p += n;
        len -= n;
    }
    return true;
}

static size_t shot_b64(char *out, const uint8_t *in, size_t len)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
                     (i + 2 < len ? in[i + 2] : 0);
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < len ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? T[v & 63] : '=';
    }
    return o;
}

static void shot_task(void *arg)
{
    (void)arg;
    const size_t need = (size_t)SHOT_W * SHOT_H * 2;
    uint8_t *frame = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = frame ? display_frame_copy(frame, need) : 0;
    if (n != need) {
        ESP_LOGW("usb_log", "shot: no %ux%u frame on this board (%u bytes)", SHOT_W, SHOT_H, (unsigned)n);
    } else if (xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
        char line[SHOT_W * 2 * 4 / 3 + 32];
        int head = snprintf(line, sizeof(line), "\n#SHOT %d %d rgb565\n", SHOT_W, SHOT_H);
        bool ok = shot_write(line, (size_t)head);
        for (int y = 0; ok && y < SHOT_H; y++) {
            head = snprintf(line, sizeof(line), "#SHOT %d ", y);
            size_t o = (size_t)head + shot_b64(line + head, frame + (size_t)y * SHOT_W * 2, SHOT_W * 2);
            line[o++] = '\n';
            ok = shot_write(line, o);
        }
        if (ok) ok = shot_write("#SHOT END\n", 10);
        tud_cdc_write_flush();
        xSemaphoreGive(s_log_lock);
        if (!ok) ESP_LOGW("usb_log", "shot: the host stopped reading");
    }
    free(frame);
    vTaskDelete(NULL);
}

static void usb_log_shot_start(void)
{
    if (xTaskCreate(shot_task, "kbx_shot", 5120, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW("usb_log", "shot: task create failed");
    }
}

static bool usb_log_handle_wifi_command(const char *line)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    if ((line[0] == 'K' || line[0] == 'k') &&
        (line[1] == 'B' || line[1] == 'b') &&
        (line[2] == 'X' || line[2] == 'x') &&
        isspace((unsigned char)line[3])) {
        line += 4;
        while (*line && isspace((unsigned char)*line)) {
            line++;
        }
    }
    if (strncasecmp(line, "wifi", 4) != 0) {
        return false;
    }
    line += 4;
    if (*line == '?') {
        net_wifi_log_status();
        return true;
    }
    if (!isspace((unsigned char)*line)) {
        return false;
    }
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    if (strcasecmp(line, "clear") == 0) {
        (void)net_wifi_clear_credentials();
        return true;
    }

    char ssid[33];
    const char *sep = strchr(line, '|');
    const char *pass = "";
    size_t ssid_len = sep ? (size_t)(sep - line) : strlen(line);
    if (ssid_len == 0 || ssid_len >= sizeof(ssid)) {
        ESP_LOGW("usb_log", "wifi: invalid SSID length %u", (unsigned)ssid_len);
        return true;
    }
    memcpy(ssid, line, ssid_len);
    ssid[ssid_len] = '\0';
    if (sep) {
        pass = sep + 1;
    }
    if (net_wifi_set_credentials(ssid, pass) != ESP_OK) {
        ESP_LOGW("usb_log", "wifi: credentials not applied");
    }
    return true;
}

/* net_ws provisioning over CDC (mirror of "KBX wifi"):
 *   KBX server <url>     speechkit-server base URL             KBX server?  status
 *   KBX token <bearer>   service bearer (never logged)         KBX token?   length only
 *   KBX agent on|off|?   open/close a voice-agent session
 *   KBX net?             combined status   KBX net clear        clear provisioning
 * The token must not contain spaces (JWT/opaque). */
static bool usb_log_handle_netws_command(const char *line)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    if ((line[0] == 'K' || line[0] == 'k') &&
        (line[1] == 'B' || line[1] == 'b') &&
        (line[2] == 'X' || line[2] == 'x') &&
        isspace((unsigned char)line[3])) {
        line += 4;
        while (*line && isspace((unsigned char)*line)) {
            line++;
        }
    }

    if (strncasecmp(line, "server", 6) == 0) {
        const char *rest = line + 6;
        if (*rest == '?') {
            net_ws_log_status();
            return true;
        }
        if (!isspace((unsigned char)*rest)) {
            return false;
        }
        while (*rest && isspace((unsigned char)*rest)) {
            rest++;
        }
        if (*rest == '\0') {
            net_ws_log_status();
        } else if (net_ws_set_server(rest) != ESP_OK) {
            ESP_LOGW("usb_log", "server: url rejected (empty/too long)");
        }
        return true;
    }

    if (strncasecmp(line, "token", 5) == 0) {
        const char *rest = line + 5;
        if (*rest == '?') {
            net_ws_log_status();
            return true;
        }
        if (!isspace((unsigned char)*rest)) {
            return false;
        }
        while (*rest && isspace((unsigned char)*rest)) {
            rest++;
        }
        if (*rest == '\0') {
            net_ws_log_status();
        } else if (net_ws_set_token(rest) != ESP_OK) {
            ESP_LOGW("usb_log", "token: rejected (empty/too long)");
        }
        return true;
    }

    if (strncasecmp(line, "route", 5) == 0) {
        const char *rest = line + 5;
        while (*rest && isspace((unsigned char)*rest)) rest++;
        if (strcasecmp(rest, "agent") == 0) net_ws_set_route_agent(true);
        else if (strcasecmp(rest, "fast") == 0) net_ws_set_route_agent(false);
        net_ws_log_status();
        return true;
    }
    if (strncasecmp(line, "agent", 5) == 0) {
        const char *rest = line + 5;
        if (*rest == '?') {
            net_ws_log_status();
            return true;
        }
        if (!isspace((unsigned char)*rest)) {
            return false;
        }
        while (*rest && isspace((unsigned char)*rest)) {
            rest++;
        }
        if (strcasecmp(rest, "on") == 0) {
            net_ws_set_agent_enabled(true);
        } else if (strcasecmp(rest, "off") == 0) {
            net_ws_set_agent_enabled(false);
        } else {
            net_ws_log_status();
        }
        return true;
    }

    /* KBX frontdoor route <utterance> | KBX frontdoor mode oneshot|conversation:
     * feeds a front-door receipt for the running session (test console). */
    if (strncasecmp(line, "frontdoor ", 10) == 0) {
        const char *kind = line + 10;
        const char *arg = kind;
        while (*arg && !isspace((unsigned char)*arg)) arg++;
        char kind_buf[8];
        size_t kind_len = (size_t)(arg - kind);
        while (*arg && isspace((unsigned char)*arg)) arg++;
        if (kind_len == 0 || kind_len >= sizeof(kind_buf)) return false;
        memcpy(kind_buf, kind, kind_len);
        kind_buf[kind_len] = '\0';
        if (!net_ws_debug_receipt(kind_buf, arg)) ESP_LOGW("usb_log", "frontdoor: not fed");
        return true;
    }

    /* KBX locale <tag> | KBX locale? : display language (en de es zh-Hans ar hi).
     * "hi" is stored but rendered as English (see i18n.h). */
    if (strncasecmp(line, "locale", 6) == 0) {
        const char *rest = line + 6;
        if (*rest == '?') {
            i18n_log_status();
            return true;
        }
        if (!isspace((unsigned char)*rest)) {
            return false;
        }
        while (*rest && isspace((unsigned char)*rest)) {
            rest++;
        }
        if (*rest == '\0' || strcmp(rest, "?") == 0) {
            i18n_log_status();
        } else if (i18n_set_locale_tag(rest) != ESP_OK) {
            ESP_LOGW("usb_log", "locale: need KBX locale <en|de|es|zh-Hans|ar|hi>");
        }
        return true;
    }

    if (strncasecmp(line, "provider", 8) == 0) {
        const char *rest = line + 8;
        if (*rest == '?') {
            net_ws_log_status();
            return true;
        }
        if (!isspace((unsigned char)*rest)) {
            return false;
        }
        while (*rest && isspace((unsigned char)*rest)) {
            rest++;
        }
        if (*rest == '\0') {
            net_ws_log_status();
        } else if (strcasecmp(rest, "clear") == 0) {
            net_ws_set_provider(NULL);
        } else if (net_ws_set_provider(rest) != ESP_OK) {
            ESP_LOGW("usb_log", "provider: rejected (too long)");
        }
        return true;
    }

    if (strncasecmp(line, "talk", 4) == 0 && (line[4] == '\0' || line[4] == '?')) {
        /* Diag: talk toggle without touch (milestone B verification) */
        if (line[4] == '?') {
            ESP_LOGI("usb_log", "talk: active=%d ready=%d",
                     (int)net_ws_talk_active(), (int)net_ws_ready_for_talk());
        } else {
            bool talking = net_ws_talk_toggle();
            ESP_LOGI("usb_log", "talk -> %s", talking ? "start" : "stop");
        }
        return true;
    }

    if (strncasecmp(line, "net", 3) == 0) {
        const char *rest = line + 3;
        if (*rest == '?') {
            net_ws_log_status();
            return true;
        }
        if (!isspace((unsigned char)*rest)) {
            return false;
        }
        while (*rest && isspace((unsigned char)*rest)) {
            rest++;
        }
        if (strcasecmp(rest, "clear") == 0) {
            net_ws_clear();
        } else {
            net_ws_log_status();
        }
        return true;
    }

    return false;
}

static const char *usb_log_strip_kbx(const char *line)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    if ((line[0] == 'K' || line[0] == 'k') &&
        (line[1] == 'B' || line[1] == 'b') &&
        (line[2] == 'X' || line[2] == 'x') &&
        isspace((unsigned char)line[3])) {
        line += 4;
        while (*line && isspace((unsigned char)*line)) {
            line++;
        }
    }
    return line;
}

/* KBX version? / ota? / ota <url> / ota-begin <bytes> [sha256] / bootloader */
static bool usb_log_handle_ota_command(const char *line)
{
    line = usb_log_strip_kbx(line);

    if (strcasecmp(line, "version?") == 0) {
        fw_ota_log_version();
        return true;
    }
    if (strcasecmp(line, "bootloader") == 0) {
        fw_ota_reboot_to_download();
        return true;
    }
    if (strcasecmp(line, "reboot") == 0) {
        fw_ota_reboot();
        return true;
    }
    if (strncasecmp(line, "ota-begin", 9) == 0) {
        const char *rest = line + 9;
        if (!isspace((unsigned char)*rest)) {
            return false;
        }
        while (*rest && isspace((unsigned char)*rest)) {
            rest++;
        }
        char *end = NULL;
        unsigned long size = strtoul(rest, &end, 10);
        if (end == rest || size == 0) {
            ESP_LOGW("usb_log", "ota-begin: need KBX ota-begin <bytes> [sha256]");
            return true;
        }
        while (*end && isspace((unsigned char)*end)) {
            end++;
        }
        const char *sha = (*end) ? end : NULL;
        if (fw_ota_begin_cdc((size_t)size, sha) != ESP_OK) {
            ESP_LOGW("usb_log", "ota-begin rejected");
        }
        return true;
    }
    if (strncasecmp(line, "ota", 3) != 0) {
        return false;
    }
    const char *rest = line + 3;
    if (*rest == '?') {
        fw_ota_log_status();
        return true;
    }
    if (!isspace((unsigned char)*rest)) {
        return false;
    }
    while (*rest && isspace((unsigned char)*rest)) {
        rest++;
    }
    if (*rest == '\0' || strcasecmp(rest, "?") == 0) {
        fw_ota_log_status();
        return true;
    }
    if (fw_ota_start_http(rest) != ESP_OK) {
        ESP_LOGW("usb_log", "ota url rejected");
    }
    return true;
}

/* One KBX line from any channel (CDC, LAN console). Serialized by
 * s_cmd_lock: the CDC callback and the LAN console task may both dispatch. */
void usb_log_exec_line(char *line)
{
    if (!s_cmd_lock) return;
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    if (usb_log_line_command_is(line, "update check")) {
        ui_notify_updates();
        (void)fw_ota_check_update();
    } else if (usb_log_line_command_is(line, "update install")) {
        (void)fw_ota_install_update();
    } else if (usb_log_line_command_is(line, "update?")) {
        fw_ota_update_status_t status;
        fw_ota_get_update_status(&status);
        ESP_LOGI("kbx_ota", "update: state=%d version=%s progress=%u detail=%s",
                 status.state, status.version, status.progress_percent, status.detail);
    } else if (usb_log_line_command_is(line, "cloud show")) {
        ui_notify_cloud_pairing();
    } else if (usb_log_line_command_is(line, "cloud pair")) {
        if (net_ws_use_cloud() == ESP_OK && cloud_auth_start_pairing() == ESP_OK) {
            device_config_reset();   /* another account's revisions start fresh */
            ui_notify_cloud_pairing();
            ESP_LOGI("cloud_auth", "pairing started; approve on your phone");
        } else ESP_LOGW("cloud_auth", "pairing could not start");
    } else if (usb_log_line_command_is(line, "cloud forget")) {
        (void)net_ws_set_agent_enabled(false);
        esp_err_t result = cloud_auth_forget();
        device_config_reset();
        ESP_LOGI("cloud_auth", "forget: %s", esp_err_to_name(result));
    } else if (usb_log_line_command_is(line, "cloud?")) {
        cloud_auth_status_t status;
        cloud_auth_get_status(&status);
        ESP_LOGI("cloud_auth", "status: paired=%d pairing=%d expires=%d code=%s uri=%s error=%s",
                 status.paired, status.pairing, status.expires_in, status.user_code,
                 status.verification_uri_complete, status.error_code);
    } else if (usb_log_line_command_is(line, "tone") || usb_log_line_command_is(line, "testtone")) {
        audio_play_test_tone();
    } else if (usb_log_line_command_is(line, "tone24")) {
        audio_play_agent_test_tone();
    } else if (usb_log_line_command_is(line, "play?")) {
        audio_log_play_status();
    } else if (usb_log_line_command_is(line, "spkrate?")) {
        audio_log_play_status();
    } else if (usb_log_line_command_is(line, "spkrate")) {
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if ((p[0] == 'K' || p[0] == 'k') &&
            (p[1] == 'B' || p[1] == 'b') &&
            (p[2] == 'X' || p[2] == 'x') &&
            isspace((unsigned char)p[3])) {
            p += 4;
        }
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || strcmp(p, "?") == 0) {
            audio_log_play_status();
        } else {
            unsigned long hz = strtoul(p, NULL, 10);
            if (hz == 16) {
                hz = 16000;
            } else if (hz == 24) {
                hz = 24000;
            }
            if (audio_play_agent_set_src_hz((uint32_t)hz) != ESP_OK) {
                ESP_LOGW("usb_log", "spkrate needs 16 or 24 (kHz)");
            }
        }
    } else if (usb_log_line_command_is(line, "touch?")) {
        touch_log_status();
    } else if (usb_log_line_command_is(line, "coredump?")) {
        usb_log_coredump_report();
    } else if (usb_log_line_command_is(line, "coredump clear")) {
        panic_record_clear();
        esp_err_t err = esp_core_dump_image_erase();
        ESP_LOGI("kbx_core", "coredump clear: %s", esp_err_to_name(err));
    } else if (usb_log_line_command_is(line, "heap?")) {
        usb_log_heap_report();
    } else if (usb_log_line_command_is(line, "mic?")) {
        audio_log_mic_status();
    } else if (usb_log_line_command_is(line, "ui?")) {
        display_log_status();
    } else if (usb_log_line_command_is(line, "i2cscan")) {
        diag_i2c_scan_async();
    } else if (usb_log_line_command_is(line, "tpreset")) {
        diag_tp_reset_async();
    } else if (usb_log_line_command_is(line, "remote") || usb_log_line_command_is(line, "remote?")) {
        const char *args = strstr(line, "emote") + 5;
        (void)lan_console_cdc_command(args);
    } else if (usb_log_line_command_is(line, "es7210")) {
        /* KBX es7210 <reg hex> [value hex]: read or write one register. */
        unsigned reg = 0, val = 0;
        const char *p = strstr(line, "7210") + 4;
        int n = sscanf(p, "%x %x", &reg, &val);
        uint8_t got = 0;
        esp_err_t e = n >= 1 ? es7210_min_reg((uint8_t)reg, n == 2 ? (int)val : -1, &got) : ESP_ERR_INVALID_ARG;
        ESP_LOGI("kbx_es7210", "reg 0x%02x %s 0x%02x: %s", reg, n == 2 ? "<-" : "=", n == 2 ? val : got,
                 esp_err_to_name(e));
    } else if (usb_log_line_command_is(line, "duck")) {
        stop_word_set_duck_margin((float)atof(strstr(line, "duck") + 4));
    } else if (usb_log_line_command_is(line, "stop?")) {
        stop_word_log();
    } else if (usb_log_line_command_is(line, "shot")) {
        usb_log_shot_start();
    } else if (usb_log_line_command_is(line, "design")) {
        /* KBX design [classic|watch|default]: the display design (account setting
         * display.design); a change restarts the box once it is idle. */
        const char *arg = strstr(line, "design") + 6;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        kbx_design_t d;
        if (*arg == '\0' || *arg == '?') {
            ESP_LOGI("usb_log", "design: running=%s stored=%s", kbx_design_id(kbx_design()),
                     kbx_design_id(kbx_design_stored()));
        } else if (!strcmp(arg, "default")) {
            (void)kbx_design_clear();
            ESP_LOGI("usb_log", "design: running=%s stored=%s%s", kbx_design_id(kbx_design()),
                     kbx_design_id(kbx_design_stored()), kbx_design_restart_needed() ? " (restart pending)" : "");
        } else if (kbx_design_from_id(arg, &d)) {
            if (kbx_design_set(d) == ESP_OK) device_config_note_local_change();
            ESP_LOGI("usb_log", "design: running=%s stored=%s%s", kbx_design_id(kbx_design()),
                     kbx_design_id(kbx_design_stored()), kbx_design_restart_needed() ? " (restart pending)" : "");
        } else {
            ESP_LOGW("usb_log", "design: need KBX design classic|watch|default");
        }
    } else if (usb_log_line_command_is(line, "bat?")) {
        battery_log();
    } else if (usb_log_line_command_is(line, "aec?")) {
        audio_aec_log();
    } else if (usb_log_line_command_is(line, "aec type vc") || usb_log_line_command_is(line, "aec type sr")) {
        aec_stage_set_type_vc(strstr(line, "vc") != NULL);
    } else if (usb_log_line_command_is(line, "aec on")) {
        audio_aec_set_enabled(true);
    } else if (usb_log_line_command_is(line, "aec off")) {
        audio_aec_set_enabled(false);
    } else if (usb_log_line_command_is(line, "tdm?")) {
        audio_tdm_log();
    } else if (usb_log_line_command_is(line, "codec vol")) {
        /* KBX codec vol <0-100>: ES8311 DAC volume until reboot (tuning). */
        const char *p = strstr(line, "vol") + 3;
        int v = atoi(p);
        if (v < 0 || v > 100) v = -1;
        esp_err_t e = v < 0 ? ESP_ERR_INVALID_ARG : es8311_min_set_volume((uint8_t)v);
        ESP_LOGI("kbx_audio", "codec volume %d: %s", v, esp_err_to_name(e));
    } else if (usb_log_line_command_is(line, "env") || usb_log_line_command_is(line, "env?")) {
        env_sensor_cdc_command(strstr(line, "env") + 3);
    } else if (usb_log_handle_wake_command(line)) {
        /* handled */
    } else if (usb_log_handle_config_command(line)) {
        /* handled */
    } else if (edge_sync_console(line)) {
        /* handled: KBX sync? | sync now | sync base */
    } else if (usb_log_handle_wifi_command(line)) {
        /* handled */
    } else if (usb_log_handle_netws_command(line)) {
        /* handled */
    } else if (usb_log_handle_ota_command(line)) {
        /* handled */
    } else {
        (void)box_status_set_from_text(line);
    }
    xSemaphoreGive(s_cmd_lock);
}

static void usb_log_handle_rx_char(char ch)
{
    if (ch == '\r') {
        return;
    }
    if (ch == '\n') {
        if (s_rx_overflow) {
            ESP_LOGW("usb_log", "cdc line too long (>%u) - discarded (token/URL longer than "
                                "CDC_RX_LINE_MAX? not a terminal glitch)",
                     (unsigned)(sizeof(s_rx_line) - 1));
            s_rx_overflow = false;
            s_rx_len = 0;
            return;
        }
        s_rx_line[s_rx_len] = '\0';
        usb_log_exec_line(s_rx_line);
        s_rx_len = 0;
        return;
    }
    if (s_rx_len + 1 < sizeof(s_rx_line)) {
        s_rx_line[s_rx_len++] = ch;
    } else {
        s_rx_overflow = true; /* discard remainder of overlong line until newline */
    }
}

void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    if (fw_ota_cdc_active()) {
        /* OTA task owns the CDC pipe until the image is fully written. */
        return;
    }
    char buf[64];
    while (tud_cdc_available()) {
        uint32_t n = tud_cdc_read(buf, sizeof(buf));
        for (uint32_t i = 0; i < n; i++) {
            usb_log_handle_rx_char(buf[i]);
        }
    }
}
