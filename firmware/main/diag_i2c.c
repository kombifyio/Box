/*
 * kombify box - I2C diagnostics for CDC commands "KBX i2cscan" and
 * "KBX tpreset".
 *
 * Background (2026-07-11): the CST816S on I2C1 (SDA1/SCL3) @0x15 never
 * answered on Marcel's device (read_errs == polls, even during physical
 * taps). Candidates: chip on another bus/address (board revision), stuck
 * in reset, or defective. The scan probes both buses across all 7-bit
 * addresses; tpreset pulses only TCA9554-EXIO0 (likely TP_RST; EXIO1 = LCD
 * is left untouched) and then probes 0x15.
 *
 * Both commands run in a short-lived task so the TinyUSB task (CDC command
 * context) is not blocked for the scan/delay duration.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"
#include "board.h"
#include "board_identity.h"
#include "system_i2c.h"
#include "touch.h"
#include "diag_i2c.h"

static const char *TAG = "kbx_diag";

#define TP_I2C_ADDR 0x15

static void scan_bus(const char *name, i2c_master_bus_handle_t bus)
{
    if (!bus) {
        ESP_LOGI(TAG, "%s: no bus handle", name);
        return;
    }
    char found[128];
    size_t n = 0;
    int count = 0;
    for (uint16_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            count++;
            if (n < sizeof(found) - 6) {
                n += (size_t)snprintf(found + n, sizeof(found) - n, " 0x%02X", addr);
            }
        }
    }
    ESP_LOGI(TAG, "%s: %d device(s)%s", name, count, count ? found : "");
}

static void diag_scan_task(void *arg)
{
    (void)arg;
    char name[48];
    i2c_master_bus_handle_t sys = NULL;
    (void)system_i2c_get(&sys);
    snprintf(name, sizeof(name), "scan i2c0/system SDA%d/SCL%d", SYS_I2C_SDA, SYS_I2C_SCL);
    scan_bus(name, sys);
    if (touch_i2c_bus() && touch_i2c_bus() != sys) {
        scan_bus("scan touch-bus (separate)", touch_i2c_bus());
    } else {
        ESP_LOGI(TAG, "touch shares the system bus (1.85C)");
    }
    kbx_board_identity_log();   /* the boot probe, both pin pairs if it needed them */
    vTaskDelete(NULL);
}

static void diag_tp_reset_task(void *arg)
{
    (void)arg;
    i2c_master_bus_handle_t sys = NULL;
    if (system_i2c_get(&sys) != ESP_OK || !sys) {
        ESP_LOGW(TAG, "tpreset: no system bus");
        vTaskDelete(NULL);
        return;
    }
    esp_io_expander_handle_t exp = NULL;
    if (esp_io_expander_new_i2c_tca9554(sys, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &exp) != ESP_OK) {
        ESP_LOGW(TAG, "tpreset: TCA9554 unreachable");
        vTaskDelete(NULL);
        return;
    }
    esp_io_expander_set_dir(exp, IO_EXPANDER_PIN_NUM_0, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_0, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_0, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_err_t p0 = i2c_master_probe(sys, TP_I2C_ADDR, 50);
    esp_err_t p1 = touch_i2c_bus() ? i2c_master_probe(touch_i2c_bus(), TP_I2C_ADDR, 50)
                                   : ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "tpreset: EXIO0 gepulst (10ms low, 300ms wait); probe 0x15: i2c0=%s i2c1=%s",
             esp_err_to_name(p0), esp_err_to_name(p1));
    esp_io_expander_del(exp);
    vTaskDelete(NULL);
}

void diag_i2c_scan_async(void)
{
    if (xTaskCreate(diag_scan_task, "kbx_diag", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "i2cscan: task create failed");
    }
}

void diag_tp_reset_async(void)
{
    if (xTaskCreate(diag_tp_reset_task, "kbx_diag", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "tpreset: task create failed");
    }
}
