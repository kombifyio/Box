/*
 * kombify box - last panic in RTC memory. See panic_record.h.
 *
 * Wraps esp_panic_handler (-Wl,--wrap, main/CMakeLists.txt; the IDF fatal
 * errors guide's "panic handler wrapping"). The record lives in RTC slow
 * memory without init, which a panic reset does not clear, so it needs no
 * flash partition and works on boxes updated over CDC OTA.
 */
#include "panic_record.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"

#define REC_MAGIC 0x4b425850u /* "KBXP" */
#define REC_BT    12

typedef struct {
    uint32_t magic;
    uint32_t core;
    uint32_t exception;
    uint32_t pc, ps, exccause, excvaddr;
    uint32_t a[16];
    uint32_t bt[REC_BT];
    uint32_t bt_depth;
    char reason[48];
    uint32_t check;
} kbx_panic_record_t;

static RTC_NOINIT_ATTR kbx_panic_record_t s_rec;

static uint32_t IRAM_ATTR rec_check(const kbx_panic_record_t *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t c = 0x9e3779b9u;
    for (size_t i = 0; i < offsetof(kbx_panic_record_t, check) / 4; i++) c = (c ^ w[i]) * 16777619u;
    return c;
}

void __real_esp_panic_handler(panic_info_t *info);

/* Return address on the stack -> call site (windowed ABI: the top two
 * bits hold the window increment). */
static inline uint32_t IRAM_ATTR stack_pc(uint32_t pc)
{
    if (pc & 0x80000000u) pc = (pc & 0x3fffffffu) | 0x40000000u;
    return pc - 3u;
}

/* XtExcFrame word offsets (xtensa_context.h, stable): exit, pc, ps,
 * a0..a15, sar, exccause, excvaddr. */
#define XF_PC       1
#define XF_PS       2
#define XF_A0       3
#define XF_EXCCAUSE 20
#define XF_EXCVADDR 21

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.magic = REC_MAGIC;
    if (info) {
        s_rec.core = (uint32_t)info->core;
        s_rec.exception = (uint32_t)info->exception;
        const uint32_t *f = (const uint32_t *)info->frame;
        if (f) {
            s_rec.pc = f[XF_PC];
            s_rec.ps = f[XF_PS];
            s_rec.exccause = f[XF_EXCCAUSE];
            s_rec.excvaddr = f[XF_EXCVADDR];
            for (int i = 0; i < 16; i++) s_rec.a[i] = f[XF_A0 + i];
            esp_backtrace_frame_t bt = {
                .pc = f[XF_PC], .sp = f[XF_A0 + 1], .next_pc = f[XF_A0],
            };
            s_rec.bt[0] = bt.pc;
            uint32_t n = 1;
            while (n < REC_BT && bt.next_pc && esp_backtrace_get_next_frame(&bt)) {
                s_rec.bt[n++] = stack_pc(bt.pc);
            }
            s_rec.bt_depth = n;
        }
        if (info->reason) {
            for (size_t i = 0; i + 1 < sizeof(s_rec.reason) && info->reason[i]; i++) {
                s_rec.reason[i] = info->reason[i];
            }
        }
    }
    s_rec.check = rec_check(&s_rec);
    __real_esp_panic_handler(info);
}

bool panic_record_valid(void)
{
    return s_rec.magic == REC_MAGIC && s_rec.check == rec_check(&s_rec);
}

void panic_record_log(void)
{
    if (!panic_record_valid()) {
        ESP_LOGI("kbx_panic", "panic record: none");
        return;
    }
    ESP_LOGW("kbx_panic", "panic record: core=%u reason=\"%s\" pc=0x%08x exccause=%u excvaddr=0x%08x ps=0x%08x",
             (unsigned)s_rec.core, s_rec.reason, (unsigned)s_rec.pc, (unsigned)s_rec.exccause,
             (unsigned)s_rec.excvaddr, (unsigned)s_rec.ps);
    char line[160];
    size_t n = 0;
    line[0] = 0;
    for (uint32_t i = 0; i < s_rec.bt_depth && i < REC_BT && n + 12 < sizeof(line); i++) {
        n += (size_t)snprintf(line + n, sizeof(line) - n, " 0x%08x", (unsigned)s_rec.bt[i]);
    }
    ESP_LOGW("kbx_panic", "panic record: backtrace:%s", line);
    n = 0;
    line[0] = 0;
    for (int i = 0; i < 16 && n + 10 < sizeof(line); i++) {
        n += (size_t)snprintf(line + n, sizeof(line) - n, " %08x", (unsigned)s_rec.a[i]);
    }
    ESP_LOGW("kbx_panic", "panic record: a0-a15:%s", line);
}

void panic_record_clear(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
}
