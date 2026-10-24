/*
 * kombify box - round 1.85" ST77916 QSPI display bring-up + LVGL port.
 * The UI itself lives in ui_scene.c / ui_overlays.c (UI 2).
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "display.h"
#include "ui.h"
#include "system_i2c.h"
#include "st77916_init_table.h"

static const char *TAG = "kbx_display";

#define KBX_CHECK(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { \
    ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(_e)); return _e; } } while (0)


#define LCD_OPCODE_READ_CMD (0x0BULL)
static lv_display_t *s_disp;

static void backlight_init(uint8_t percent)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = (1023 * percent) / 100,
        .hpoint = 0,
    };
    ledc_channel_config(&c);
}

static void lcd_reset_via_tca9554(void)
{
    i2c_master_bus_handle_t bus = NULL;
    if (system_i2c_get(&bus) != ESP_OK) { ESP_LOGE(TAG, "i2c bus init failed"); return; }
    esp_io_expander_handle_t exp = NULL;
    esp_err_t err = esp_io_expander_new_i2c_tca9554(bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &exp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 not found (%s) - LCD reset skipped", esp_err_to_name(err));
        return;
    }
    esp_io_expander_set_dir(exp, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

size_t display_frame_copy(uint8_t *out, size_t cap)
{
    (void)out; (void)cap;
    return 0;   /* the 1.85C panel has no CPU-side frame */
}

void display_log_status(void)
{
    ui_scene_log_status();
}

esp_err_t display_init(void)
{
    lcd_reset_via_tca9554();

    spi_bus_config_t bus = {
        .data0_io_num = LCD_DATA0,
        .data1_io_num = LCD_DATA1,
        .sclk_io_num = LCD_PCLK,
        .data2_io_num = LCD_DATA2,
        .data3_io_num = LCD_DATA3,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    KBX_CHECK(spi_bus_initialize(LCD_QSPI_HOST, &bus, SPI_DMA_CH_AUTO));

    /* probe panel revision at 3 MHz (read reg 0x04) */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 3 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = 1 },
    };
    KBX_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_QSPI_HOST, &io_cfg, &io));

    uint8_t rev[4] = {0};
    int lcd_cmd = (0x04 & 0xff) << 8 | (LCD_OPCODE_READ_CMD << 24);
    esp_err_t err = esp_lcd_panel_io_rx_param(io, lcd_cmd, rev, sizeof(rev));
    ESP_LOGI(TAG, "panel reg 0x04: %02x %02x %02x %02x (%s)", rev[0], rev[1], rev[2], rev[3], esp_err_to_name(err));
    esp_lcd_panel_io_del(io);

    /* real IO at 80 MHz */
    io_cfg.pclk_hz = 80 * 1000 * 1000;
    KBX_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_QSPI_HOST, &io_cfg, &io));

    st77916_vendor_config_t vendor = {
        .flags = { .use_qspi_interface = 1 },
    };
    if (rev[0] == 0x00 && rev[1] == 0x02 && rev[2] == 0x7F && rev[3] == 0x7F) {
        vendor.init_cmds = vendor_specific_init_new;
        vendor.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(st77916_lcd_init_cmd_t);
        ESP_LOGI(TAG, "using vendor init table (panel rev 2)");
    } else {
        ESP_LOGI(TAG, "using driver default init (panel rev 1)");
    }

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel = NULL;
    KBX_CHECK(esp_lcd_new_panel_st77916(io, &pcfg, &panel));
    KBX_CHECK(esp_lcd_panel_reset(panel));
    KBX_CHECK(esp_lcd_panel_init(panel));
    KBX_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    backlight_init(85);

    /* LVGL: a 5 ms tick keeps the CPU permanently awake. 16 ms is enough
     * for status UI + aura; audio runs independently on I2S/UAC. */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.timer_period_ms = 16;
    KBX_CHECK(lvgl_port_init(&port_cfg));
    const lvgl_port_display_cfg_t dcfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&dcfg);

    lvgl_port_lock(0);
    ui_scene_build(s_disp);
    lvgl_port_unlock();
    ui_scene_health_start();
    ESP_LOGI(TAG, "display up");
    return ESP_OK;
}
