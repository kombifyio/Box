/*
 * kombify box - Waveshare RLCD 4.2 (ST7305 reflective LCD, 400x300, 1 bpp,
 * no backlight) bring-up + LVGL.
 *
 * LVGL keeps rendering the dark RGB565 UI 2 theme. The flush converts each
 * area to the panel's 1-bit frame: luminance is inverted (the dark ground
 * becomes paper, light text becomes ink) and ordered-dithered (Bayer 4x4)
 * so the brand teal, secondary text and the orb gradient keep their weight.
 * The whole 15000-byte frame goes out in one SPI write after the last area.
 *
 * Init sequence and pixel packing: waveshareteam/ESP32-S3-RLCD-4.2 @ d8bf781
 * examples/esp-idf/07_Audio_Test/components/port_bsp/display_bsp.cpp
 * (identical in xiaozhi-esp32 boards/waveshare/esp32-s3-rlcd-4.2).
 */
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "display.h"
#include "ui.h"

static const char *TAG = "kbx_display";

#define KBX_CHECK(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { \
    ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(_e)); return _e; } } while (0)

#define RLCD_PCLK_HZ     (20 * 1000 * 1000)
#define RLCD_FRAME_BYTES (LCD_H_RES * LCD_V_RES / 8)
/* SPI EDMA reads PSRAM only in whole 64-byte cache lines; a shorter tail
 * makes the driver copy the frame to internal RAM, which fails when it is
 * short. The 40 extra bytes repeat the frame start: the ST7305 wraps to
 * the window start after its last byte and rewrites identical data. */
#define RLCD_SEND_BYTES  ((RLCD_FRAME_BYTES + 63U) & ~63U)
#define RLCD_DRAW_LINES  60

static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;
static uint8_t *s_frame;              /* panel frame, PSRAM (EDMA) */
static SemaphoreHandle_t s_frame_free; /* given when the SPI frame write is done */
static volatile uint32_t s_frames, s_frame_us_max;

/* Bayer 4x4 thresholds in a narrow band (luma 52..112): the reflective
 * panel has no grey, and wide dithering made small text grainy.
 * Grounds and pill fills (luma < 52) stay paper, every
 * text colour including the secondary grey (luma ~146) and the brand teal
 * becomes solid ink; only the band in between (orb glow, anti-aliased
 * edges) is dithered. */
#define BAYER(v) ((uint8_t)(52 + (v) * 4))
static const uint8_t k_bayer[4][4] = {
    { BAYER(0), BAYER(8), BAYER(2), BAYER(10) },
    { BAYER(12), BAYER(4), BAYER(14), BAYER(6) },
    { BAYER(3), BAYER(11), BAYER(1), BAYER(9) },
    { BAYER(15), BAYER(7), BAYER(13), BAYER(5) },
};

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[10];
    uint16_t delay_ms;
} rlcd_init_cmd_t;

static const rlcd_init_cmd_t k_init[] = {
    { 0xD6, 2, { 0x17, 0x02 }, 0 },
    { 0xD1, 1, { 0x01 }, 0 },
    { 0xC0, 2, { 0x11, 0x04 }, 0 },
    { 0xC1, 4, { 0x69, 0x69, 0x69, 0x69 }, 0 },
    { 0xC2, 4, { 0x19, 0x19, 0x19, 0x19 }, 0 },
    { 0xC4, 4, { 0x4B, 0x4B, 0x4B, 0x4B }, 0 },
    { 0xC5, 4, { 0x19, 0x19, 0x19, 0x19 }, 0 },
    { 0xD8, 2, { 0x80, 0xE9 }, 0 },
    { 0xB2, 1, { 0x02 }, 0 },
    { 0xB3, 10, { 0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45 }, 0 },
    { 0xB4, 8, { 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45 }, 0 },
    { 0x62, 3, { 0x32, 0x03, 0x1F }, 0 },
    { 0xB7, 1, { 0x13 }, 0 },
    { 0xB0, 1, { 0x64 }, 0 },
    { 0x11, 0, { 0 }, 200 },   /* sleep out */
    { 0xC9, 1, { 0x00 }, 0 },
    { 0x36, 1, { 0x48 }, 0 },
    { 0x3A, 1, { 0x11 }, 0 },
    { 0xB9, 1, { 0x20 }, 0 },
    { 0xB8, 1, { 0x29 }, 0 },
    { 0x21, 0, { 0 }, 0 },     /* inversion on */
    { 0x2A, 2, { 0x12, 0x2A }, 0 },
    { 0x2B, 2, { 0x00, 0xC7 }, 0 },
    { 0x35, 1, { 0x00 }, 0 },  /* TE on */
    { 0xD0, 1, { 0xFF }, 0 },
    { 0x38, 0, { 0 }, 0 },     /* idle off */
    { 0x29, 0, { 0 }, 0 },     /* display on */
};

static bool rlcd_frame_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io; (void)edata; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_free, &hp);
    return hp == pdTRUE;
}

/* Landscape packing (display_bsp.cpp InitLandscapeLUT): two columns by four
 * rows per byte, rows counted from the bottom; bit set = white. */
static inline void rlcd_put(int x, int y, bool ink)
{
    const int inv_y = LCD_V_RES - 1 - y;
    const uint32_t index = (uint32_t)(x >> 1) * (LCD_V_RES / 4) + (uint32_t)(inv_y >> 2);
    const uint8_t bit = (uint8_t)(1U << (7 - (((inv_y & 3) << 1) | (x & 1))));
    if (ink) s_frame[index] &= (uint8_t)~bit;
    else s_frame[index] |= bit;
}

static void rlcd_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *px = (const uint16_t *)px_map;
    xSemaphoreTake(s_frame_free, portMAX_DELAY);
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            const uint16_t c = *px++;
            /* RGB565 -> 8-bit luma (BT.601 weights) */
            const uint32_t r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
            const uint32_t luma = (r * 77 + g * 150 + b * 29) >> 8;
            rlcd_put(x, y, luma > k_bayer[y & 3][x & 3]);
        }
    }
    if (lv_display_flush_is_last(disp)) {
        static const uint8_t col[] = { 0x12, 0x2A }, row[] = { 0x00, 0xC7 };
        int64_t t0 = esp_timer_get_time();
        esp_lcd_panel_io_tx_param(s_io, 0x2A, col, sizeof(col));
        esp_lcd_panel_io_tx_param(s_io, 0x2B, row, sizeof(row));
        memcpy(s_frame + RLCD_FRAME_BYTES, s_frame, RLCD_SEND_BYTES - RLCD_FRAME_BYTES);
        if (esp_lcd_panel_io_tx_color(s_io, 0x2C, s_frame, RLCD_SEND_BYTES) != ESP_OK) {
            xSemaphoreGive(s_frame_free);
        }
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > s_frame_us_max) s_frame_us_max = us;
        s_frames++;
    } else {
        xSemaphoreGive(s_frame_free);
    }
    lv_display_flush_ready(disp);
}

size_t display_frame_copy(uint8_t *out, size_t cap)
{
    if (!s_frame || !out || cap < RLCD_FRAME_BYTES) return 0;
    /* The frame is complete between flushes: take it like a flush does. */
    if (xSemaphoreTake(s_frame_free, pdMS_TO_TICKS(500)) != pdTRUE) return 0;
    memcpy(out, s_frame, RLCD_FRAME_BYTES);
    xSemaphoreGive(s_frame_free);
    return RLCD_FRAME_BYTES;
}

void display_log_status(void)
{
    ESP_LOGI(TAG, "rlcd: frames=%u queue_us_max=%u", (unsigned)s_frames, (unsigned)s_frame_us_max);
    ui_scene_log_status();
}

esp_err_t display_init(void)
{
    s_frame_free = xSemaphoreCreateBinary();
    /* PSRAM, cache-line aligned: esp_lcd sends it with SPI EDMA straight
     * from PSRAM (SPI_TRANS_DMA_USE_PSRAM), which keeps 15 KB of internal
     * RAM for the wake word next to the echo canceller. */
    s_frame = heap_caps_aligned_calloc(64, 1, RLCD_SEND_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame_free || !s_frame) return ESP_ERR_NO_MEM;
    memset(s_frame, 0xFF, RLCD_FRAME_BYTES);   /* paper */
    xSemaphoreGive(s_frame_free);

    const spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = RLCD_SEND_BYTES,
    };
    KBX_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = RLCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = rlcd_frame_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        /* send the PSRAM frame by EDMA; without it esp_lcd copies every
         * frame to a 15 KB internal buffer and fails when RAM is short */
        .flags.psram_dma_direct = 1,
    };
    KBX_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));

    gpio_reset_pin(LCD_RST);
    gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
        KBX_CHECK(esp_lcd_panel_io_tx_param(s_io, k_init[i].cmd, k_init[i].len ? k_init[i].data : NULL,
                                            k_init[i].len));
        if (k_init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(k_init[i].delay_ms));
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.timer_period_ms = 16;
    /* 5 KB: the LVGL task peaks at ~4.3 KB here, and internal RAM is short
     * next to the echo canceller. */
    port_cfg.task_stack = 5120;
    KBX_CHECK(lvgl_port_init(&port_cfg));

    const size_t draw_bytes = LCD_H_RES * RLCD_DRAW_LINES * sizeof(uint16_t);
    void *draw = heap_caps_malloc(draw_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!draw) return ESP_ERR_NO_MEM;
    lvgl_port_lock(0);
    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, draw, NULL, draw_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, rlcd_flush);
    ui_scene_build(s_disp);
    lvgl_port_unlock();
    ui_scene_health_start();
    ESP_LOGI(TAG, "display up: ST7305 RLCD %dx%d 1bpp, SPI %d MHz", LCD_H_RES, LCD_V_RES,
             RLCD_PCLK_HZ / 1000000);
    return ESP_OK;
}
