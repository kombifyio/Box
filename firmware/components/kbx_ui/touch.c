/*
 * kombify box - CST816S touch controller (shared system I2C).
 *
 * Gestures on the round 360x360 surface: tap = start/stop talk (or interrupt
 * playback), hold = mute latch, top-bezel swipe-down = Quick Settings,
 * v-swipe = volume. Tap does not open a status card.
 * INT-driven with 30 ms motion polling; the CST816S is kept permanently
 * awake via DisAutoSleep (0xFE) because release-based classification needs
 * dense samples (a sleeping chip NACKs and turned every swipe into a tap).
 * Init is non-fatal: on any failure the box keeps running without touch.
 */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "board.h"
#include "audio.h"
#include "net_ws.h"
#include "system_i2c.h"
#include "talk_action.h"
#include "touch.h"
#include "display_profile.h"
#include "ui.h"

static const char *TAG = "kbx_touch";

static esp_lcd_touch_handle_t s_tp = NULL;
static TaskHandle_t s_task = NULL;
static i2c_master_bus_handle_t s_bus = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static bool s_awake_cfg = false; /* DisAutoSleep written successfully */

/* CST816S: DisAutoSleep (reg 0xFE) != 0 keeps the chip permanently awake.
 * Needed because a sleeping chip NACKs every I2C read and gesture
 * classification then sees only 1 sample per stroke (swipe == tap).
 * The write ACKs only while the chip is awake (just after reset or
 * during a touch) — caller treats false as "try again later". */
static bool touch_write_awake_cfg(void)
{
    if (!s_io) {
        return false;
    }
    const uint8_t dis_auto_sleep = 0x01;
    return esp_lcd_panel_io_tx_param(s_io, 0xFE, &dis_auto_sleep, 1) == ESP_OK;
}

i2c_master_bus_handle_t touch_i2c_bus(void)
{
    return s_bus;
}

/* Child lock: set by the panel (LVGL context), read in the touch task + UI.
 * Simple bool store, no lock needed (worst case: one gesture ignored too
 * early/late). */
static volatile bool s_locked = false;

bool touch_is_locked(void) { return s_locked; }
void touch_set_locked(bool locked) { s_locked = locked; }

/* Diagnostics for "KBX touch?": records where touch is stuck (init error
 * vs. controller delivers no events vs. gestures arriving). */
static esp_err_t s_init_err = ESP_ERR_NOT_FINISHED; /* never initialized */
static volatile uint32_t s_diag_polls;     /* task-loop iterations */
static volatile uint32_t s_diag_presses;   /* press edges */
static volatile uint32_t s_diag_actions;   /* classified gestures */
static volatile uint32_t s_diag_read_errs; /* NACKs (chip asleep) */

void touch_log_status(void)
{
    int x = 0, y = 0;
    bool pressed = false;
    touch_get_point(&x, &y, &pressed);
    ESP_LOGI(TAG, "status: init=%s task=%s awake_cfg=%d polls=%u presses=%u actions=%u read_errs=%u last=(%d,%d)%s",
             s_init_err == ESP_OK ? "ok" : esp_err_to_name(s_init_err),
             s_task ? "up" : "none", (int)s_awake_cfg,
             (unsigned)s_diag_polls, (unsigned)s_diag_presses,
             (unsigned)s_diag_actions, (unsigned)s_diag_read_errs,
             x, y, pressed ? " pressed" : "");
}

/* State published for the LVGL indev. Packed into ONE 32-bit word so the
 * LVGL reader never sees a half-updated coordinate (x/y/pressed as three
 * separate volatiles could tear -> click on the wrong widget). Aligned
 * 32-bit store/load is atomic on the S3. Layout: bit31=pressed,
 * bits12..23=y, bits0..11=x (0..4095). */
static volatile uint32_t s_pub_point;

#define TP_PACK(x, y, pressed) \
    (((uint32_t)((pressed) ? 1u : 0u) << 31) | \
     (((uint32_t)(y) & 0xFFFu) << 12) | ((uint32_t)(x) & 0xFFFu))

void touch_get_point(int *x, int *y, bool *pressed)
{
    uint32_t p = s_pub_point;
    if (x) *x = (int)(p & 0xFFFu);
    if (y) *y = (int)((p >> 12) & 0xFFFu);
    if (pressed) *pressed = (p >> 31) != 0;
}

static void IRAM_ATTR touch_isr_cb(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    BaseType_t hp = pdFALSE;
    if (s_task) {
        vTaskNotifyGiveFromISR(s_task, &hp);
    }
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

/* gesture thresholds */
#define TP_MOVE_MAX     56      /* px: still counts as a stationary tap/hold */
#define TP_SWIPE_MIN    80      /* px of vertical travel to count as a swipe */
#define TP_LONG_US      800000  /* press >800 ms stationary = mute latch */
#define TP_DEBOUNCE_US  250000
#define TP_VOLUME_STEP  8
#define TP_SETTINGS_EDGE 48     /* only the top bezel starts settings, never the orb */
/* Orb hit circle from the display profile (stage centre, touch radius);
 * taps there are talk and never reach LVGL widgets. */
#define TP_ORB_CX       ((int)kbx_display_profile()->stage_cx)
#define TP_ORB_CY       ((int)kbx_display_profile()->stage_cy)
#define TP_ORB_R        ((int)kbx_display_profile()->touch_r)
#define TP_WAKE_BURST   20      /* 10 ms retries after INT until the chip reads */
#define TP_MULTITAP_GAP_US 700000  /* max pause between unlock taps */
#define TP_UNLOCK_TAPS  3       /* triple-tap unlocks child lock */
#define TP_SLIDE_RANGE  315     /* px of finger travel = full panel slide (0..1000) */

#define TP_PANEL_LATCH  500     /* drag progress (0..1000) at which the panel
                                 * LATCHES open on release — even if the finger
                                 * drifted back or touch sampling dropped
                                 * (user report 2026-08-21: panel opened briefly
                                 * then closed again). */

bool touch_point_on_orb(int x, int y)
{
    int dx = x - TP_ORB_CX;
    int dy = y - TP_ORB_CY;
    return (dx * dx + dy * dy) <= (TP_ORB_R * TP_ORB_R);
}

/* The whole round surface is one control.
 * tap -> talk start/stop (or interrupt playback), hold (stationary >800 ms)
 * -> mute latch, vertical swipe -> volume. Mute always works, even without
 * Wi-Fi. A tap never opens the status/settings card. */
static void touch_task(void *arg)
{
    (void)arg;
    int64_t last_action_us = 0;
    int64_t press_start_us = 0;
    int64_t last_tap_us = 0;
    int unlock_taps = 0;
    bool was_pressed = false;
    bool stroke_lvgl_owned = false;       /* panel was open at press start */
    bool panel_drag = false;              /* this stroke is dragging the bottom sheet */
    bool panel_candidate = false;         /* press in top edge, not a drag yet */
    bool hold_muted = false;              /* this stroke latched mute */
    int drag_base = 0, drag_anchor_y = 0, drag_progress = 0, drag_max = 0;
    int x0 = 0, y0 = 0, xl = 0, yl = 0;   /* first + last coords while pressed */
    int wake_burst = 0;
    TickType_t poll_ticks = pdMS_TO_TICKS(30);
    for (;;) {
        /* wake on INT, or poll so we can track motion while held. The first
         * read right after INT wake can still NACK — then retry briefly in a
         * 10 ms burst so the stroke start (x0/y0!) is not lost. Only if the
         * chip stays silent longer AND nothing is pressed (asleep,
         * DisAutoSleep not active) do we fall back to 250 ms. */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, poll_ticks);
        if (!s_tp) {
            continue;
        }
        if (notified) {
            wake_burst = TP_WAKE_BURST;
        }
        esp_err_t read_err = esp_lcd_touch_read_data(s_tp);
        if (read_err != ESP_OK) {
            s_diag_read_errs++;
        } else if (!s_awake_cfg) {
            /* Chip is awake right now (touch) -> disable auto-sleep
             * permanently if init NACKed (chip may have been asleep). */
            s_awake_cfg = touch_write_awake_cfg();
            if (s_awake_cfg) {
                ESP_LOGI(TAG, "auto-sleep disabled (0xFE) via first touch");
            }
        }
        uint16_t x[1] = {0}, y[1] = {0}, strength[1] = {0};
        uint8_t cnt = 0;
        bool pressed = esp_lcd_touch_get_coordinates(s_tp, x, y, strength, &cnt, 1) && cnt > 0;
        if (read_err == ESP_OK || pressed) {
            poll_ticks = pdMS_TO_TICKS(30);
        } else if (wake_burst > 0) {
            wake_burst--;
            poll_ticks = pdMS_TO_TICKS(10);
        } else {
            poll_ticks = pdMS_TO_TICKS(250);
        }
        int64_t now = esp_timer_get_time();
        s_diag_polls++;

        if (pressed) {
            if (!was_pressed) {
                s_diag_presses++;
                press_start_us = now;
                x0 = x[0]; y0 = y[0];
                stroke_lvgl_owned = ui_settings_is_open();
                /* Stroke in the top edge (handle/shade zone) drags the panel
                 * following the finger: down opens, up closes. Otherwise
                 * normal gestures / LVGL controls. */
                hold_muted = false;
                panel_drag = false;
                panel_candidate = (y0 < TP_SETTINGS_EDGE && !touch_point_on_orb(x0, y0) &&
                                   !s_locked);
                if (panel_candidate) {
                    drag_anchor_y = y0;
                    drag_base = ui_panel_progress();
                    drag_progress = drag_base;
                    drag_max = drag_base;
                }
            }
            xl = x[0]; yl = y[0];
            s_pub_point = TP_PACK(x[0], y[0], true);
            if (panel_candidate && !panel_drag && !hold_muted) {
                int ddy = yl - y0;
                int adx = xl - x0; if (adx < 0) adx = -adx;
                int ady = yl - y0; if (ady < 0) ady = -ady;
                if (ddy > TP_SWIPE_MIN && ady > adx) {
                    panel_drag = true;
                    ui_notify_panel_drag(drag_base);
                }
            }
            if (panel_drag) {
                /* Progress 1:1 with the finger: down (yl>anchor) increases. */
                int p = drag_base + (yl - drag_anchor_y) * 1000 / TP_SLIDE_RANGE;
                if (p < 0) p = 0; else if (p > 1000) p = 1000;
                drag_progress = p;
                if (p > drag_max) drag_max = p;
                ui_notify_panel_drag(p);
            } else if (!s_locked && !stroke_lvgl_owned && !hold_muted) {
                int64_t held = now - press_start_us;
                int adx = xl - x0; if (adx < 0) adx = -adx;
                int ady = yl - y0; if (ady < 0) ady = -ady;
                if (held > TP_LONG_US && adx < TP_MOVE_MAX && ady < TP_MOVE_MAX) {
                    hold_muted = true;
                    talk_action_hold_mute();
                }
            }
        } else {
            /* keep last coordinate (LVGL expects that), pressed=false */
            s_pub_point = TP_PACK(xl, yl, false);
        }

        if (!pressed && was_pressed) {
            /* release edge: classify gesture from duration + travel. */
            int64_t dur = now - press_start_us;
            int dx = xl - x0, dy = yl - y0;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            bool is_tap = (adx < TP_MOVE_MAX && ady < TP_MOVE_MAX && dur < TP_LONG_US);
            bool vswipe = (ady > TP_SWIPE_MIN && ady > adx);

            /* Per-stroke diagnostics: shows WHY a gesture did (not) fire —
             * especially whether the panel drag armed and with which y0/net.
             * Readable via CDC. */
            ESP_LOGD(TAG, "stroke y0=%d yl=%d dy=%d dur=%dms drag=%d locked=%d panel_open=%d",
                     y0, yl, dy, (int)(dur / 1000), (int)panel_drag, (int)s_locked,
                     (int)ui_settings_is_open());

            if (panel_drag) {
                /* Release the bottom sheet: decide by DIRECTION, not absolute
                 * position — when closing the finger has little travel at the
                 * top (y0<90), so a position threshold would never fire.
                 * If it was open: close only on a noticeable upward pull; if
                 * it was closed: open only on a noticeable downward pull. */
                int net = yl - drag_anchor_y;   /* + down, - up */
                bool started_open = (drag_base >= 500);
                /* Latch: once pulled past halfway it stays open —
                 * independent of where the finger is on release
                 * (drag_max instead of release position, see TP_PANEL_LATCH). */
                bool open = started_open ? (net > -TP_SWIPE_MIN)
                                         : (drag_max >= TP_PANEL_LATCH || net > TP_SWIPE_MIN);
                ui_notify_panel_drag_end(open);
                last_action_us = now;
                ESP_LOGI(TAG, "panel drag end -> %s (net=%d max=%d)", open ? "open" : "close", net, drag_max);
            } else if (s_locked) {
                /* Child lock: only the triple-tap unlocks; everything else
                 * is ignored (voice/host keep running). */
                if (is_tap) {
                    if (now - last_tap_us > TP_MULTITAP_GAP_US) {
                        unlock_taps = 0;
                    }
                    unlock_taps++;
                    last_tap_us = now;
                    if (unlock_taps >= TP_UNLOCK_TAPS) {
                        unlock_taps = 0;
                        s_locked = false;
                        ui_notify_lock_tap(0);   /* brief "unlocked" */
                        ESP_LOGI(TAG, "child lock unlocked (%dx tap)", TP_UNLOCK_TAPS);
                    } else {
                        ui_notify_lock_tap(TP_UNLOCK_TAPS - unlock_taps); /* "N left" */
                    }
                } else {
                    unlock_taps = 0;   /* non-tap breaks the sequence */
                }
            } else if (ui_settings_is_open() && vswipe && dy < 0 &&
                       now - last_action_us > TP_DEBOUNCE_US) {
                /* Panel open + fast flick up from the content (y0>=90, no
                 * drag) -> close (animated). Opening is the finger-following
                 * drag from the top edge/handle zone. */
                last_action_us = now;
                ui_notify_settings(false);
                ESP_LOGI(TAG, "flick up -> settings close");
            } else if (hold_muted) {
                last_action_us = now;
                hold_muted = false;
            } else if (!stroke_lvgl_owned && now - last_action_us > TP_DEBOUNCE_US) {
                /* remaining gestures only with the panel closed */
                s_diag_actions++;
                if (vswipe) {
                    last_action_us = now;
                    int up = (dy < 0) ? 1 : -1;   /* screen y grows downward */
                    uint32_t vol = audio_adjust_volume(up > 0 ? TP_VOLUME_STEP : -TP_VOLUME_STEP);
                    ui_notify_swipe(up);
                    ESP_LOGI(TAG, "swipe %s -> volume=%u", up > 0 ? "up" : "down", (unsigned)vol);
                } else if (is_tap) {
                    last_action_us = now;
                    ui_notify_tap();
                    talk_action_tap();
                }
            }
        }
        was_pressed = pressed;
    }
}

esp_err_t touch_init(void)
{
    s_init_err = ESP_FAIL;
    /* 1.85C: CST816 shares system I2C (SDA11/SCL10) with TCA9554/codecs.
     * The bus belongs to system_i2c.c; this file only borrows it. */
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = system_i2c_get(&bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "system i2c unavailable: %s", esp_err_to_name(err));
        s_init_err = err;
        return err;
    }
    s_bus = bus;

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_lcd_panel_io_handle_t io = NULL;
    err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch panel io init failed: %s", esp_err_to_name(err));
        s_init_err = err;
        return err;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,   // CST816S reset shares the LCD reset via TCA9554
        .int_gpio_num = TP_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .interrupt_callback = touch_isr_cb,
    };
    err = esp_lcd_touch_new_i2c_cst816s(io, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cst816s init failed: %s (mute button disabled)", esp_err_to_name(err));
        s_tp = NULL;
        s_init_err = err;
        return err;
    }
    s_io = io;

    /* Right after boot reset the chip is usually still awake — first try
     * to disable auto-sleep. If it NACKs (already asleep), the task retries
     * on the first touch. */
    s_awake_cfg = touch_write_awake_cfg();
    if (s_awake_cfg) {
        ESP_LOGI(TAG, "auto-sleep disabled (0xFE) at init");
    }

    /* A sleeping CST816S NACKs every poll read; unfiltered, i2c/lcd_panel/
     * CST816S error logs flood the console (~165 lines/s at 30 ms poll ->
     * UART saturates -> task-WDT reboot loop, observed 2026-07-11).
     * Errors are counted in s_diag_read_errs instead ("KBX touch?").
     * Codec/TCA9554 problems on the system bus are still reported by the
     * kbx_es7210/kbx_es8311/kbx_i2c wrapper tags. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("CST816S", ESP_LOG_NONE);

    if (xTaskCreate(touch_task, "kbx_touch", 3072, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "touch task create failed");
        s_init_err = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "touch up: CST816S on system I2C (SDA%d/SCL%d), tap=talk hold=mute",
             SYS_I2C_SDA, SYS_I2C_SCL);
    s_init_err = ESP_OK;
    return ESP_OK;
}
