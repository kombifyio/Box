#include "c6_sd.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "diskio_impl.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "driver/gpio.h"
#include "nvs.h"

static const char *TAG = "c6_sd";

#define P_MOSI GPIO_NUM_10
#define P_SCK GPIO_NUM_11
#define P_MISO GPIO_NUM_18
#define P_CS GPIO_NUM_6

#define MOUNT "/sdcard"
#define DIR_ROOT MOUNT "/kbx"
#define DIR_WAKE DIR_ROOT "/wake"
#define DIR_LOG DIR_ROOT "/log"
#define CSV_PATH DIR_ROOT "/wake.csv"

#define WAKE_MAX_FRAMES 100            /* 2 s of 20 ms frames */
#define LOG_FILE_BYTES (256 * 1024)
#define LOG_FILES 4
#define MSGBUF_BYTES 4096
#define RETRY_US ((int64_t)60 * 1000 * 1000)

/* ── Bit-banged SPI-mode SD card ──────────────────────────────────────── */

static int s_half_nops = 24;           /* 24: ~250 kHz for the card's init, 0: as fast as the loop goes */
static bool s_block_addr;
static uint32_t s_sectors;
static volatile bool s_card_ok;

static inline void spin(void)
{
    for (int i = 0; i < s_half_nops; ++i) __asm__ volatile("nop");
}

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 0; i < 8; ++i) {
        gpio_ll_set_level(&GPIO, P_MOSI, (out & 0x80) != 0);
        out <<= 1;
        spin();
        gpio_ll_set_level(&GPIO, P_SCK, 1);
        spin();
        in = (uint8_t)((in << 1) | gpio_ll_get_level(&GPIO, P_MISO));
        gpio_ll_set_level(&GPIO, P_SCK, 0);
    }
    return in;
}

static void cs(bool low)
{
    gpio_ll_set_level(&GPIO, P_CS, !low);
    if (!low) xfer(0xFF);              /* the card releases MISO after one more clock byte */
}

static bool wait_ready(int ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        if (xfer(0xFF) == 0xFF) return true;
        if (ms > 5) vTaskDelay(1);
    }
    return false;
}

static uint8_t sd_cmd(uint8_t c, uint32_t arg)
{
    if (c != 0 && !wait_ready(500)) return 0xFF;
    xfer(0x40 | c);
    xfer((uint8_t)(arg >> 24));
    xfer((uint8_t)(arg >> 16));
    xfer((uint8_t)(arg >> 8));
    xfer((uint8_t)arg);
    xfer(c == 0 ? 0x95 : c == 8 ? 0x87 : 0x01);   /* CRC is only checked for CMD0 and CMD8 */
    uint8_t r = 0xFF;
    for (int i = 0; i < 10; ++i) {
        r = xfer(0xFF);
        if (!(r & 0x80)) break;
    }
    return r;
}

static uint8_t sd_acmd(uint8_t c, uint32_t arg)
{
    sd_cmd(55, 0);
    return sd_cmd(c, arg);
}

static bool read_data(uint8_t *buf, size_t n)
{
    int64_t end = esp_timer_get_time() + 300 * 1000;
    uint8_t tok;
    do {
        tok = xfer(0xFF);
    } while (tok == 0xFF && esp_timer_get_time() < end);
    if (tok != 0xFE) return false;
    for (size_t i = 0; i < n; ++i) buf[i] = xfer(0xFF);
    xfer(0xFF);
    xfer(0xFF);
    return true;
}

static void pins_init(void)
{
    gpio_config_t out = { .pin_bit_mask = (1ULL << P_MOSI) | (1ULL << P_SCK) | (1ULL << P_CS), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_config_t in = { .pin_bit_mask = 1ULL << P_MISO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
    gpio_ll_set_level(&GPIO, P_CS, 1);
    gpio_ll_set_level(&GPIO, P_MOSI, 1);
    gpio_ll_set_level(&GPIO, P_SCK, 0);
}

static void pins_release(void)
{
    gpio_reset_pin(P_MOSI);
    gpio_reset_pin(P_SCK);
    gpio_reset_pin(P_MISO);
    gpio_reset_pin(P_CS);
}

static esp_err_t card_init(void)
{
    s_card_ok = false;
    s_half_nops = 24;
    pins_init();
    gpio_ll_set_level(&GPIO, P_CS, 1);
    for (int i = 0; i < 12; ++i) xfer(0xFF);       /* 96 clocks with CS high */
    cs(true);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && r != 0x01; ++i) r = sd_cmd(0, 0);
    if (r != 0x01) {
        cs(false);
        return ESP_ERR_NOT_FOUND;                  /* no card, or not answering in SPI mode */
    }
    bool v2 = false;
    if (sd_cmd(8, 0x1AA) == 0x01) {
        uint8_t ocr[4];
        for (int i = 0; i < 4; ++i) ocr[i] = xfer(0xFF);
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) {
            cs(false);
            return ESP_ERR_NOT_SUPPORTED;
        }
        v2 = true;
    }
    int64_t end = esp_timer_get_time() + 1500 * 1000;
    do {
        r = sd_acmd(41, v2 ? 0x40000000u : 0);
        if (r) vTaskDelay(pdMS_TO_TICKS(10));
    } while (r != 0 && esp_timer_get_time() < end);
    if (r != 0) {
        cs(false);
        return ESP_ERR_TIMEOUT;
    }
    s_block_addr = false;
    if (v2 && sd_cmd(58, 0) == 0) {
        uint8_t ocr[4];
        for (int i = 0; i < 4; ++i) ocr[i] = xfer(0xFF);
        s_block_addr = (ocr[0] & 0x40) != 0;
    }
    if (!s_block_addr) sd_cmd(16, 512);
    uint8_t csd[16];
    if (sd_cmd(9, 0) != 0 || !read_data(csd, sizeof(csd))) {
        cs(false);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((csd[0] >> 6) == 1) {
        s_sectors = (((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9]) + 1;
        s_sectors *= 1024;
    } else {
        uint32_t c_size = ((uint32_t)(csd[6] & 3) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);
        uint32_t bl = csd[5] & 0x0F;
        s_sectors = ((c_size + 1) << (mult + 2)) << bl >> 9;
    }
    cs(false);
    s_half_nops = 2;                               /* a few MHz once initialised */
    s_card_ok = true;
    return ESP_OK;
}

static DSTATUS d_status(BYTE pdrv)
{
    (void)pdrv;
    return s_card_ok ? 0 : STA_NOINIT;
}

static DSTATUS d_init(BYTE pdrv)
{
    return d_status(pdrv);
}

static DRESULT d_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!s_card_ok) return RES_NOTRDY;
    for (UINT i = 0; i < count; ++i) {
        uint32_t addr = s_block_addr ? (uint32_t)(sector + i) : (uint32_t)((sector + i) * 512);
        cs(true);
        bool ok = sd_cmd(17, addr) == 0 && read_data(buf + (size_t)i * 512, 512);
        cs(false);
        if (!ok) {
            s_card_ok = false;                     /* the card task unmounts and remounts */
            return RES_ERROR;
        }
    }
    return RES_OK;
}

static DRESULT d_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!s_card_ok) return RES_NOTRDY;
    for (UINT i = 0; i < count; ++i) {
        uint32_t addr = s_block_addr ? (uint32_t)(sector + i) : (uint32_t)((sector + i) * 512);
        cs(true);
        bool ok = false;
        if (sd_cmd(24, addr) == 0) {
            xfer(0xFF);
            xfer(0xFE);
            for (int b = 0; b < 512; ++b) xfer(buf[(size_t)i * 512 + b]);
            xfer(0xFF);
            xfer(0xFF);
            ok = (xfer(0xFF) & 0x1F) == 0x05 && wait_ready(500);
        }
        cs(false);
        if (!ok) {
            s_card_ok = false;
            return RES_ERROR;
        }
    }
    return RES_OK;
}

static DRESULT d_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = s_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buf = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* ── Mount ────────────────────────────────────────────────────────────── */

static bool s_mounted;
static BYTE s_pdrv = 0xFF;
static FATFS *s_fs;
static esp_err_t s_last_err = ESP_OK;

static esp_err_t sd_mount(void)
{
    if (s_mounted) return ESP_OK;
    esp_err_t err = card_init();
    if (err != ESP_OK) {
        pins_release();
        return err;
    }
    if (ff_diskio_get_drive(&s_pdrv) != ESP_OK) {
        pins_release();
        s_card_ok = false;
        return ESP_ERR_NO_MEM;
    }
    static const ff_diskio_impl_t impl = { .init = d_init, .status = d_status, .read = d_read, .write = d_write, .ioctl = d_ioctl };
    ff_diskio_register(s_pdrv, &impl);
    char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
    const esp_vfs_fat_conf_t conf = { .base_path = MOUNT, .fat_drive = drv, .max_files = 4 };
    err = esp_vfs_fat_register(&conf, &s_fs);
    if (err == ESP_OK && f_mount(s_fs, drv, 1) != FR_OK) {
        err = ESP_ERR_INVALID_STATE;               /* not a FAT volume: the card is never formatted here */
        /* FatFs keeps the pointer it was given even when the mount fails; the
         * VFS frees the FATFS object (and its sector buffer) on unregister, so
         * detach it first or the next access uses freed memory. */
        f_mount(NULL, drv, 0);
        esp_vfs_fat_unregister_path(MOUNT);
        s_fs = NULL;
    }
    if (err != ESP_OK) {
        ff_diskio_unregister(s_pdrv);
        pins_release();
        s_card_ok = false;
        return err;
    }
    mkdir(DIR_ROOT, 0777);
    mkdir(DIR_WAKE, 0777);
    mkdir(DIR_LOG, 0777);
    s_mounted = true;
    ESP_LOGI(TAG, "card mounted: %u MB, %s addressing", (unsigned)(s_sectors / 2048), s_block_addr ? "block" : "byte");
    return ESP_OK;
}

static void sd_unmount(void)
{
    if (!s_mounted) return;
    char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
    f_mount(NULL, drv, 0);
    esp_vfs_fat_unregister_path(MOUNT);
    ff_diskio_unregister(s_pdrv);
    pins_release();
    s_mounted = s_card_ok = false;
}

/* ── Messages to the card task ────────────────────────────────────────── */

enum { M_BEGIN = 1, M_PCM, M_END, M_LOG };

typedef struct {
    uint8_t type;
    union {
        struct { char phrase[24]; float score; uint32_t id; } begin;
        struct { char verdict[12]; int32_t verify_ms; uint32_t drops; } end;
        int16_t pcm[320];
        char log[192];
    };
} msg_t;

static MessageBufferHandle_t s_mb;
static TaskHandle_t s_task;
static volatile bool s_on;
static volatile uint32_t s_drops;
static volatile bool s_wake_active;            /* a candidate is being recorded */
static uint32_t s_wake_frames_sent;
static volatile uint32_t s_clip_drops;         /* frames of the running candidate that did not fit the queue */
static volatile bool s_unmount_req;
static vprintf_like_t s_prev_vprintf;
static uint32_t s_clip_seq;                    /* id of the latest candidate (voice task) */
/* A close that found the queue full waits here for the card task, tagged
 * with its candidate so it never closes a later one. */
static struct { bool valid; char verdict[12]; int32_t verify_ms; uint32_t drops; uint32_t id; } s_pend_end;

/* A message buffer allows several writers only when every send is serialised
 * and never blocks: the log hook (any task), the voice task and the console
 * all send through here, in one critical section, with block time 0. */
static portMUX_TYPE s_send_mux = portMUX_INITIALIZER_UNLOCKED;

static bool post(const msg_t *m, size_t used)
{
    MessageBufferHandle_t mb = s_mb;
    if (!mb) return false;
    portENTER_CRITICAL(&s_send_mux);
    size_t sent = xMessageBufferSend(mb, m, used, 0);
    portEXIT_CRITICAL(&s_send_mux);
    if (sent != used) {
        s_drops++;
        return false;
    }
    return true;
}

static int log_hook(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    if (s_on && s_mb && xTaskGetCurrentTaskHandle() != s_task) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        static msg_t m;
        portENTER_CRITICAL(&mux);
        m.type = M_LOG;
        int n = vsnprintf(m.log, sizeof(m.log), fmt, copy);
        if (n > 0) {
            size_t len = (size_t)(n < (int)sizeof(m.log) ? n : (int)sizeof(m.log) - 1);
            post(&m, offsetof(msg_t, log) + len + 1);
        }
        portEXIT_CRITICAL(&mux);
    }
    va_end(copy);
    return ret;
}

/* ── The card task ────────────────────────────────────────────────────── */

static FILE *s_wav;
static uint32_t s_wav_bytes;
static char s_wav_name[24];
static uint32_t s_seq;
static char s_phrase[24];
static float s_score;
static FILE *s_logf;
static uint32_t s_log_bytes, s_log_unsynced;
static int64_t s_retry_us;
static uint32_t s_cur_clip;                    /* candidate the open WAV belongs to */
static bool s_row_due;                         /* the open candidate still owes its wake.csv row */
static char s_owed_row[160];                   /* a row a card error kept for the next mount */

static void wav_header(uint8_t h[44], uint32_t data)
{
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t s16 = 1;
    memcpy(h + 20, &s16, 2);
    memcpy(h + 22, &s16, 2);
    v = 16000;
    memcpy(h + 24, &v, 4);
    v = 32000;
    memcpy(h + 28, &v, 4);
    s16 = 2;
    memcpy(h + 32, &s16, 2);
    s16 = 16;
    memcpy(h + 34, &s16, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
}

static void csv_row(char *buf, size_t n, const char *verdict, int verify_ms, uint32_t drops)
{
    time_t now = time(NULL);
    snprintf(buf, n, "%u,%u,%lld,%s,%.3f,%s,%d,%s,%u\n", (unsigned)s_seq, (unsigned)(esp_timer_get_time() / 1000000),
             now > 1600000000 ? (long long)now : 0LL, s_phrase, (double)s_score, verdict, verify_ms,
             s_wav_bytes ? s_wav_name : "-", (unsigned)drops);
}

static void csv_append(const char *row)
{
    FILE *csv = fopen(CSV_PATH, "a");
    if (!csv) return;
    if (ftell(csv) == 0) fputs("seq,uptime_s,epoch,phrase,score,verdict,verify_ms,file,dropped_frames\n", csv);
    fputs(row, csv);
    fclose(csv);
}

/* A read or write error cleared s_card_ok: close the files, unmount and let
 * the next use try a fresh mount after a short pause. */
static void card_check(void)
{
    if (!s_mounted || s_card_ok) return;
    if (s_wav) {
        fclose(s_wav);
        s_wav = NULL;
    }
    if (s_row_due) {
        /* The card takes no writes now: the row waits for the next mount. */
        csv_row(s_owed_row, sizeof(s_owed_row), "card_error", 0, s_clip_drops);
        s_row_due = false;
    }
    if (s_logf) {
        fclose(s_logf);
        s_logf = NULL;
    }
    ESP_LOGW(TAG, "card error: unmounting, remount in 5 s");
    sd_unmount();
    s_retry_us = esp_timer_get_time() + 5 * 1000 * 1000;
}

static bool ensure_mounted(void)
{
    card_check();
    /* After "sd off", lines still queued must not mount the card again. */
    if (!s_on) return false;
    if (s_mounted) return true;
    if (esp_timer_get_time() < s_retry_us) return false;
    esp_err_t err = sd_mount();
    if (err != ESP_OK) {
        if (err != s_last_err) ESP_LOGW(TAG, "no usable card (%s); retry in 60 s", esp_err_to_name(err));
        s_last_err = err;
        s_retry_us = esp_timer_get_time() + RETRY_US;
        return false;
    }
    s_last_err = ESP_OK;
    if (s_owed_row[0]) {
        csv_append(s_owed_row);
        s_owed_row[0] = '\0';
    }
    return true;
}

static void log_rotate(void)
{
    if (s_logf) {
        fclose(s_logf);
        s_logf = NULL;
    }
    char a[40], b[40];
    for (int i = LOG_FILES - 1; i > 0; --i) {
        snprintf(a, sizeof(a), DIR_LOG "/log.%d.txt", i - 1);
        snprintf(b, sizeof(b), DIR_LOG "/log.%d.txt", i);
        remove(b);
        rename(a, b);
    }
}

static void log_write(const char *line)
{
    if (!ensure_mounted()) return;
    if (!s_logf) {
        s_logf = fopen(DIR_LOG "/log.0.txt", "a");
        if (!s_logf) return;
        fseek(s_logf, 0, SEEK_END);
        s_log_bytes = (uint32_t)ftell(s_logf);
    }
    size_t n = strlen(line);
    fwrite(line, 1, n, s_logf);
    s_log_bytes += (uint32_t)n;
    s_log_unsynced += (uint32_t)n;
    if (s_log_unsynced > 2048) {
        fflush(s_logf);
        s_log_unsynced = 0;
    }
    if (s_log_bytes >= LOG_FILE_BYTES) {
        log_rotate();
        s_log_bytes = 0;
    }
}

static void wav_close(const char *verdict, int verify_ms, uint32_t drops);

static void wav_open(const char *phrase, float score)
{
    /* A close that never arrived must not leak the file or lose the row. */
    wav_close("lost", 0, 0);
    strlcpy(s_phrase, phrase, sizeof(s_phrase));
    s_score = score;
    if (!ensure_mounted()) return;
    nvs_handle_t h;
    s_seq = 0;
    if (nvs_open("c6_sd", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "seq", &s_seq);
        nvs_set_u32(h, "seq", ++s_seq);
        nvs_commit(h);
        nvs_close(h);
    }
    snprintf(s_wav_name, sizeof(s_wav_name), "w%05u.wav", (unsigned)s_seq);
    char path[64];
    snprintf(path, sizeof(path), DIR_WAKE "/%s", s_wav_name);
    s_wav = fopen(path, "wb");
    s_wav_bytes = 0;
    s_row_due = true;
    if (s_wav) {
        uint8_t h44[44];
        wav_header(h44, 0);
        fwrite(h44, 1, sizeof(h44), s_wav);
    }
}

static void wav_close(const char *verdict, int verify_ms, uint32_t drops)
{
    if (s_wav) {
        uint8_t h44[44];
        wav_header(h44, s_wav_bytes);
        fseek(s_wav, 0, SEEK_SET);
        fwrite(h44, 1, sizeof(h44), s_wav);
        fclose(s_wav);
        s_wav = NULL;
    }
    /* One row per candidate: a later close of the same one (an END after
     * "aborted" or a card error) writes none. */
    if (!s_row_due) return;
    s_row_due = false;
    if (!s_mounted) return;
    char row[160];
    csv_row(row, sizeof(row), verdict, verify_ms, drops);
    csv_append(row);
}

static void handle_end(const char *verdict, int verify_ms, uint32_t drops)
{
    wav_close(verdict, verify_ms, drops);
    if (s_logf) {
        fflush(s_logf);
        s_log_unsynced = 0;
    }
}

/* Applies a parked close to its own candidate. before_id > 0 takes it only
 * when it belongs to an earlier candidate than the one about to open; 0
 * leaves a close whose BEGIN is still queued parked. */
static void take_parked_end(uint32_t before_id)
{
    char verdict[12];
    int ms;
    uint32_t drops, id;
    portENTER_CRITICAL(&s_send_mux);
    if (!s_pend_end.valid || s_pend_end.id >= (before_id ? before_id : s_cur_clip + 1)) {
        portEXIT_CRITICAL(&s_send_mux);
        return;
    }
    memcpy(verdict, s_pend_end.verdict, sizeof(verdict));
    ms = s_pend_end.verify_ms;
    drops = s_pend_end.drops;
    id = s_pend_end.id;
    s_pend_end.valid = false;
    portEXIT_CRITICAL(&s_send_mux);
    if (id == s_cur_clip) handle_end(verdict, ms, drops);
}

static void card_task(void *arg)
{
    (void)arg;
    static msg_t m;
    for (;;) {
        size_t n = xMessageBufferReceive(s_mb, &m, sizeof(m), pdMS_TO_TICKS(500));
        if (n == 0 && s_logf && s_log_unsynced) {
            fflush(s_logf);
            s_log_unsynced = 0;
        }
        if (n > 0) {
            card_check();
            switch (m.type) {
            case M_BEGIN:
                take_parked_end(m.begin.id);
                wav_open(m.begin.phrase, m.begin.score);
                s_cur_clip = m.begin.id;
                break;
            case M_PCM:
                if (s_wav) s_wav_bytes += (uint32_t)fwrite(m.pcm, 1, 640, s_wav);
                break;
            case M_END:
                handle_end(m.end.verdict, m.end.verify_ms, m.end.drops);
                break;
            case M_LOG:
                log_write(m.log);
                break;
            }
        }
        /* A close the queue had no room for: take it once everything queued
         * before it has been written. */
        if (s_pend_end.valid && xMessageBufferIsEmpty(s_mb)) take_parked_end(0);
        if (s_unmount_req && !s_on) {
            s_unmount_req = false;
            if (s_logf) {
                fclose(s_logf);
                s_logf = NULL;
            }
            wav_close("aborted", 0, s_clip_drops);
            sd_unmount();
        }
    }
}

/* ── API ──────────────────────────────────────────────────────────────── */

static void start(void)
{
    if (!s_mb) s_mb = xMessageBufferCreate(MSGBUF_BYTES);
    if (!s_mb) {
        ESP_LOGW(TAG, "no RAM for the card buffer");
        return;
    }
    if (!s_task) xTaskCreate(card_task, "c6_sd", 4096, NULL, 2, &s_task);
    if (!s_prev_vprintf) s_prev_vprintf = esp_log_set_vprintf(log_hook);
    s_on = true;
}

void c6_sd_init(void)
{
    nvs_handle_t h;
    uint8_t on = 0;
    if (nvs_open("c6_sd", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "on", &on);
        nvs_close(h);
    }
    if (on) start();
}

bool c6_sd_enabled(void)
{
    return s_on;
}

esp_err_t c6_sd_set_enabled(bool on)
{
    nvs_handle_t h;
    if (nvs_open("c6_sd", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "on", on);
        nvs_commit(h);
        nvs_close(h);
    }
    if (on) {
        s_unmount_req = false;
        s_retry_us = 0;
        start();
    } else if (s_on) {
        s_on = false;
        s_unmount_req = true;                      /* the card task closes and unmounts within 500 ms */
    }
    return s_on == on ? ESP_OK : ESP_ERR_NO_MEM;
}

void c6_sd_wake_begin(const char *phrase, float score)
{
    s_wake_active = false;
    if (!s_on) return;
    msg_t m = { .type = M_BEGIN };
    strlcpy(m.begin.phrase, phrase ? phrase : "", sizeof(m.begin.phrase));
    m.begin.score = score;
    m.begin.id = ++s_clip_seq;
    s_wake_frames_sent = 0;
    s_clip_drops = 0;
    s_wake_active = post(&m, offsetof(msg_t, begin) + sizeof(m.begin));
}

void c6_sd_wake_frame(const int16_t *pcm, size_t samples)
{
    if (!s_on || !s_wake_active || s_wake_frames_sent >= WAKE_MAX_FRAMES || samples != 320) return;
    static msg_t m;
    m.type = M_PCM;
    memcpy(m.pcm, pcm, 640);
    s_wake_frames_sent++;
    /* This runs on the wake-verification path: never wait. A frame that does
     * not fit is dropped and counted in the clip's dropped_frames. */
    if (!post(&m, offsetof(msg_t, pcm) + 640)) s_clip_drops++;
}

void c6_sd_wake_end(const char *verdict, int verify_ms)
{
    if (!s_on || !s_wake_active) return;
    s_wake_active = false;
    msg_t m = { .type = M_END };
    strlcpy(m.end.verdict, verdict ? verdict : "", sizeof(m.end.verdict));
    m.end.verify_ms = verify_ms;
    m.end.drops = s_clip_drops;
    if (!post(&m, offsetof(msg_t, end) + sizeof(m.end))) {
        /* No room: park the close for the card task, never block here. */
        portENTER_CRITICAL(&s_send_mux);
        strlcpy(s_pend_end.verdict, m.end.verdict, sizeof(s_pend_end.verdict));
        s_pend_end.verify_ms = verify_ms;
        s_pend_end.drops = m.end.drops;
        s_pend_end.id = s_clip_seq;
        s_pend_end.valid = true;
        portEXIT_CRITICAL(&s_send_mux);
    }
}

void c6_sd_log_status(void)
{
    ESP_LOGI(TAG, "sd: capture=%s mounted=%d card_mb=%u last_err=%s queue_drops=%u clip_drops=%u wake_frames=%u seq=%u",
             s_on ? "on" : "off", s_mounted, (unsigned)(s_sectors / 2048), esp_err_to_name(s_last_err),
             (unsigned)s_drops, (unsigned)s_clip_drops, (unsigned)s_wake_frames_sent, (unsigned)s_seq);
}

void c6_sd_log_list(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "sd: not mounted (capture %s)", s_on ? "on, no card yet" : "off");
        return;
    }
    DIR *d = opendir(DIR_WAKE);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (n++ < 20) ESP_LOGI(TAG, "sd: %s", e->d_name);
    }
    if (d) closedir(d);
    ESP_LOGI(TAG, "sd: %d wake clips", n);
}
