#include "c6_power.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_power";

#define AXP2101_REG_COMMON_CFG 0x10
#define AXP2101_SOFT_PWROFF    0x01

esp_err_t c6_power_off(void)
{
    ESP_LOGI(TAG, "powering off");
    (void)bsp_audio_poweramp_enable(false);
    (void)bsp_display_brightness_set(0);
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_master_dev_handle_t pmu = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_PMU_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &pmu);
    if (err == ESP_OK) {
        uint8_t reg = AXP2101_REG_COMMON_CFG, val = 0;
        err = i2c_master_transmit_receive(pmu, &reg, 1, &val, 1, 100);
        if (err == ESP_OK) {
            uint8_t cmd[2] = { AXP2101_REG_COMMON_CFG, (uint8_t)(val | AXP2101_SOFT_PWROFF) };
            err = i2c_master_transmit(pmu, cmd, sizeof(cmd), 100);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        i2c_master_bus_rm_device(pmu);
    }
    /* Still running: the PMU refused or is absent. Sleep with the screen off
     * so the panel is not left lit; USB replug or reset wakes the board. */
    ESP_LOGW(TAG, "PMU power-off did not take effect (%s); deep sleep instead", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
    return err;
}
