#include "es8311_min.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kbx_es8311";

#define ES8311_I2C_TIMEOUT_MS 100

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t es8311_write(uint8_t reg, uint8_t value)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t data[2] = { reg, value };
    return i2c_master_transmit(s_dev, data, sizeof(data), ES8311_I2C_TIMEOUT_MS);
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *value)
{
    if (!s_dev || !value) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, ES8311_I2C_TIMEOUT_MS);
}

static esp_err_t es8311_find_addr(i2c_master_bus_handle_t bus, uint8_t *out_addr)
{
    static const uint8_t addrs[] = { 0x18, 0x19 };
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (i2c_master_probe(bus, addrs[i], 50) == ESP_OK) {
            *out_addr = addrs[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t es8311_min_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    uint8_t reg32 = volume == 0 ? 0 : (uint8_t)(((uint32_t)volume * 256 / 100) - 1);
    return es8311_write(0x32, reg32);
}

esp_err_t es8311_min_mute(bool muted)
{
    uint8_t reg31 = 0;
    esp_err_t err = es8311_read(0x31, &reg31);
    if (err != ESP_OK) {
        return err;
    }
    if (muted) {
        reg31 |= 0x60;
    } else {
        reg31 &= (uint8_t)~0x60;
    }
    return es8311_write(0x31, reg31);
}

esp_err_t es8311_min_init(i2c_master_bus_handle_t bus, uint32_t sample_rate_hz, uint8_t volume, uint8_t *out_addr)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz != 48000) {
        ESP_LOGE(TAG, "unsupported sample rate: %u", (unsigned)sample_rate_hz);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t addr = 0;
    esp_err_t err = es8311_find_addr(bus, &addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 not found on I2C: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_dev) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400000,
        };
        err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add ES8311@0x%02x failed: %s", addr, esp_err_to_name(err));
            return err;
        }
    }

#define ES8311_W(reg, val) do { \
        err = es8311_write((reg), (val)); \
        if (err != ESP_OK) { \
            ESP_LOGE(TAG, "write 0x%02x=0x%02x failed: %s", (reg), (val), esp_err_to_name(err)); \
            return err; \
        } \
    } while (0)
#define ES8311_R(reg, out) do { \
        err = es8311_read((reg), &(out)); \
        if (err != ESP_OK) { \
            ESP_LOGE(TAG, "read 0x%02x failed: %s", (reg), esp_err_to_name(err)); \
            return err; \
        } \
    } while (0)

    ES8311_W(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    ES8311_W(0x00, 0x00);
    ES8311_W(0x00, 0x80);

    // 48 kHz, 16-bit I2S slave, MCLK=12.288 MHz from GPIO2.
    ES8311_W(0x01, 0x3F);
    uint8_t reg06 = 0;
    ES8311_R(0x06, reg06);
    reg06 &= (uint8_t)~0x20;
    ES8311_W(0x06, reg06);

    uint8_t reg02 = 0;
    ES8311_R(0x02, reg02);
    ES8311_W(0x02, reg02 & 0x07);
    ES8311_W(0x03, 0x10);
    ES8311_W(0x04, 0x10);
    ES8311_W(0x05, 0x00);
    ES8311_R(0x06, reg06);
    ES8311_W(0x06, (reg06 & 0xE0) | 0x03);
    uint8_t reg07 = 0;
    ES8311_R(0x07, reg07);
    ES8311_W(0x07, reg07 & 0xC0);
    ES8311_W(0x08, 0xFF);

    uint8_t reg00 = 0;
    ES8311_R(0x00, reg00);
    ES8311_W(0x00, reg00 & 0xBF);
    ES8311_W(0x09, 0x0C);
    ES8311_W(0x0A, 0x0C);

    ES8311_W(0x0D, 0x01);
    ES8311_W(0x0E, 0x02);
    ES8311_W(0x12, 0x00);
    ES8311_W(0x13, 0x10);
    ES8311_W(0x1C, 0x6A);
    ES8311_W(0x37, 0x08);
    ES8311_W(0x14, 0x1A);

    ESP_RETURN_ON_ERROR(es8311_min_set_volume(volume), TAG, "set volume failed");
    ESP_RETURN_ON_ERROR(es8311_min_mute(false), TAG, "unmute failed");

    uint8_t id1 = 0, id2 = 0, ver = 0;
    (void)es8311_read(0xFD, &id1);
    (void)es8311_read(0xFE, &id2);
    (void)es8311_read(0xFF, &ver);

#undef ES8311_W
#undef ES8311_R

    if (out_addr) {
        *out_addr = addr;
    }
    ESP_LOGI(TAG, "ES8311 up at 0x%02x: id=%02x%02x ver=%02x volume=%u", addr, id1, id2, ver, volume);
    return ESP_OK;
}
