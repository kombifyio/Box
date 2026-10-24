#include "es7210_min.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kbx_es7210";
static bool s_tdm;
static i2c_master_dev_handle_t s_dev;

void es7210_min_use_tdm(bool tdm)
{
    s_tdm = tdm;
}

#define ES7210_I2C_TIMEOUT_MS 100

static esp_err_t es7210_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };
    return i2c_master_transmit(dev, data, sizeof(data), ES7210_I2C_TIMEOUT_MS);
}

static esp_err_t es7210_find_addr(i2c_master_bus_handle_t bus, uint8_t *out_addr)
{
    static const uint8_t addrs[] = { 0x40, 0x41, 0x42, 0x43 };
    for (size_t i = 0; i < sizeof(addrs); i++) {
        esp_err_t err = i2c_master_probe(bus, addrs[i], 50);
        if (err == ESP_OK) {
            *out_addr = addrs[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t es7210_min_init(i2c_master_bus_handle_t bus, uint32_t sample_rate_hz, uint8_t *out_addr)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz != 48000) {
        ESP_LOGE(TAG, "unsupported sample rate: %u", (unsigned)sample_rate_hz);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t addr = 0;
    esp_err_t err = es7210_find_addr(bus, &addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 not found on I2C: %s", esp_err_to_name(err));
        return err;
    }

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add ES7210@0x%02x failed: %s", addr, esp_err_to_name(err));
        return err;
    }
    s_dev = dev;

#define ES7210_W(reg, val) do { \
        err = es7210_write(dev, (reg), (val)); \
        if (err != ESP_OK) { \
            ESP_LOGE(TAG, "write 0x%02x=0x%02x failed: %s", (reg), (val), esp_err_to_name(err)); \
            return err; \
        } \
    } while (0)

    ES7210_W(0x00, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(1));
    ES7210_W(0x00, 0x32);
    ES7210_W(0x09, 0x30);
    ES7210_W(0x0A, 0x30);
    ES7210_W(0x23, 0x2A);
    ES7210_W(0x22, 0x0A);
    ES7210_W(0x21, 0x2A);
    ES7210_W(0x20, 0x0A);

    // 16-bit I2S slots. Normal: MIC1/MIC2 on SDOUT1. TDM (RLCD 4.2): mode 1
    // puts all four ADCs on SDOUT1 in order MIC1, MIC2, MIC3, MIC4; measured
    // on the board with KBX es7210/tdm? (mode 2 left MIC3 off the line).
    ES7210_W(0x11, 0x60);
    ES7210_W(0x12, s_tdm ? 0x01 : 0x00);

    ES7210_W(0x40, 0xC3);
    ES7210_W(0x41, 0x70);
    ES7210_W(0x42, 0x70);
    /* Select MIC1-4 (bit4) + 37.5 dB analog PGA (code 14). 30 dB left
     * desk speech below the listen threshold and SpeechKit VAD. */
    /* TDM (RLCD 4.2): 30 dB instead of 37.5 dB keeps the microphones linear
     * at full speaker volume, so echo cancellation can remove the reply;
     * the 7.5 dB return after the AEC (KBX_AEC_POST_GAIN_Q8). */
    ES7210_W(0x43, s_tdm ? 0x1A : 0x1E);
    ES7210_W(0x44, s_tdm ? 0x1A : 0x1E);
    /* TDM (RLCD 4.2): MIC3 is the speaker loopback, not a microphone. 12 dB
     * puts the test tone at about -17 dBFS (37.5 dB clipped it) over a
     * -86 dBFS floor: a clean AEC reference. */
    ES7210_W(0x45, s_tdm ? 0x14 : 0x1E);
    ES7210_W(0x46, 0x1E);
    ES7210_W(0x47, 0x08);
    ES7210_W(0x48, 0x08);
    ES7210_W(0x49, 0x08);
    ES7210_W(0x4A, 0x08);

    // 48 kHz with 12.288 MHz MCLK (sample_rate * 256).
    ES7210_W(0x07, 0x20);
    ES7210_W(0x02, 0xC1);
    ES7210_W(0x04, 0x01);
    ES7210_W(0x05, 0x00);

    /* +12 dB digital (0xBF is 0 dB, 0.5 dB steps). Analog 37.5 dB plus
     * this is what makes conversational speech move the orb / USB VAD. */
    ES7210_W(0x1B, 0xD7);
    ES7210_W(0x1C, 0xD7);
    ES7210_W(0x1D, 0xD7);
    ES7210_W(0x1E, 0xD7);

    ES7210_W(0x06, 0x04);
    ES7210_W(0x4B, 0x0F);
    ES7210_W(0x4C, 0x0F);
    ES7210_W(0x00, 0x71);
    ES7210_W(0x00, 0x41);

#undef ES7210_W

    if (out_addr) {
        *out_addr = addr;
    }
    ESP_LOGI(TAG, "ES7210 up at 0x%02x: 48 kHz, 16-bit %s", addr, s_tdm ? "TDM 4 slots" : "I2S stereo");
    return ESP_OK;
}

esp_err_t es7210_min_reg(uint8_t reg, int value, uint8_t *out)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (value >= 0) return es7210_write(s_dev, reg, (uint8_t)value);
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, ES7210_I2C_TIMEOUT_MS);
}
