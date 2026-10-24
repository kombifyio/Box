#include "system_i2c.h"

#include "board.h"
#include "esp_log.h"

static const char *TAG = "kbx_i2c";

static i2c_master_bus_handle_t s_system_bus = NULL;

esp_err_t system_i2c_get(i2c_master_bus_handle_t *out_bus)
{
    if (!out_bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_system_bus) {
        *out_bus = s_system_bus;
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SYS_I2C_SDA,
        .scl_io_num = SYS_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_system_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "system I2C init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "system I2C up: port0 SDA%d/SCL%d", SYS_I2C_SDA, SYS_I2C_SCL);
    *out_bus = s_system_bus;
    return ESP_OK;
}
