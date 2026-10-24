/*
 * kombify box - room climate from the onboard SHTC3 (RLCD 4.2, I2C 0x70).
 *
 * A low-priority task measures every 30 s (normal mode, T first, no clock
 * stretching) and keeps the chip asleep in between. The board warms the
 * sensor a little; KBX env offset <tenths> stores a correction in NVS and
 * the humidity follows it to the room temperature.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "env_sensor.h"
#include "system_i2c.h"

static const char *TAG = "kbx_env";

#define SHTC3_ADDR      0x70
#define ENV_PERIOD_MS   30000
#define ENV_NVS_NS      "kbx_env"
#define ENV_NVS_OFFSET  "t_off"

static i2c_master_dev_handle_t s_dev;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_temp_c = NAN, s_rh = NAN;
static int16_t s_offset_tenths;   /* added to the raw temperature */
static uint32_t s_reads, s_errors;

static uint8_t crc8(const uint8_t *d, size_t n)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* The sensor sits warmer than the room: the same water vapour reads as a
 * lower relative humidity there. Carry it to the room temperature with the
 * Magnus saturation pressure (RH_room = RH_sensor * es(T_sensor) / es(T_room)). */
static float rh_at(float rh, float t_sensor, float t_room)
{
    if (t_sensor == t_room) return rh;
    const float es_s = expf(17.62f * t_sensor / (243.12f + t_sensor));
    const float es_r = expf(17.62f * t_room / (243.12f + t_room));
    const float v = rh * es_s / es_r;
    return v > 100.0f ? 100.0f : v < 0.0f ? 0.0f : v;
}

static esp_err_t shtc3_cmd(uint16_t cmd)
{
    const uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)cmd };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t shtc3_measure(float *temp_c, float *rh)
{
    esp_err_t err = shtc3_cmd(0x3517);   /* wake up */
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(1));
    err = shtc3_cmd(0x7866);             /* measure: normal mode, T first */
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(15));
        uint8_t d[6];
        err = i2c_master_receive(s_dev, d, sizeof(d), 50);
        if (err == ESP_OK && (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5])) err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK) {
            *temp_c = -45.0f + 175.0f * (float)((d[0] << 8) | d[1]) / 65536.0f;
            *rh = 100.0f * (float)((d[3] << 8) | d[4]) / 65536.0f;
        }
    }
    (void)shtc3_cmd(0xB098);             /* sleep */
    return err;
}

static void env_task(void *arg)
{
    (void)arg;
    for (;;) {
        float t = NAN, rh = NAN;
        esp_err_t err = shtc3_measure(&t, &rh);
        taskENTER_CRITICAL(&s_mux);
        if (err == ESP_OK) {
            const float room = t + (float)s_offset_tenths / 10.0f;
            s_temp_c = room;
            s_rh = rh_at(rh, t, room);
            s_reads++;
        } else {
            s_errors++;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (err != ESP_OK) ESP_LOGW(TAG, "SHTC3 read failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(ENV_PERIOD_MS));
    }
}

esp_err_t env_sensor_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = system_i2c_get(&bus);
    if (err != ESP_OK) return err;
    err = i2c_master_probe(bus, SHTC3_ADDR, 50);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SHTC3 at 0x%02x: %s", SHTC3_ADDR, esp_err_to_name(err));
        return err;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) return err;
    nvs_handle_t nvs;
    if (nvs_open(ENV_NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_i16(nvs, ENV_NVS_OFFSET, &s_offset_tenths);
        nvs_close(nvs);
    }
    if (xTaskCreate(env_task, "kbx_env", 3072, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "SHTC3 up, every %d s, offset %d.%d C", ENV_PERIOD_MS / 1000, s_offset_tenths / 10,
             abs(s_offset_tenths % 10));
    return ESP_OK;
}

bool env_sensor_get(float *temp_c, float *rh)
{
    taskENTER_CRITICAL(&s_mux);
    const float t = s_temp_c, h = s_rh;
    taskEXIT_CRITICAL(&s_mux);
    if (isnan(t) || isnan(h)) return false;
    if (temp_c) *temp_c = t;
    if (rh) *rh = h;
    return true;
}

void env_sensor_cdc_command(const char *args)
{
    while (*args == ' ') args++;
    if (!strncmp(args, "offset", 6)) {
        const int tenths = atoi(args + 6);
        if (tenths < -100 || tenths > 100) {
            ESP_LOGW(TAG, "offset in tenths of a degree, -100..100");
            return;
        }
        nvs_handle_t nvs;
        esp_err_t err = nvs_open(ENV_NVS_NS, NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            err = nvs_set_i16(nvs, ENV_NVS_OFFSET, (int16_t)tenths);
            if (err == ESP_OK) err = nvs_commit(nvs);
            nvs_close(nvs);
        }
        taskENTER_CRITICAL(&s_mux);
        if (err == ESP_OK) {
            if (!isnan(s_temp_c)) {
                const float sensor = s_temp_c - (float)s_offset_tenths / 10.0f;
                const float rh_sensor = rh_at(s_rh, s_temp_c, sensor);
                s_temp_c = sensor + (float)tenths / 10.0f;
                s_rh = rh_at(rh_sensor, sensor, s_temp_c);
            }
            s_offset_tenths = (int16_t)tenths;
        }
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "offset %d tenths: %s", tenths, esp_err_to_name(err));
        return;
    }
    float t, h;
    if (env_sensor_get(&t, &h)) {
        ESP_LOGI(TAG, "env: %.1f C %.0f %%RH (offset %d tenths, reads=%u errors=%u)", (double)t, (double)h,
                 s_offset_tenths, (unsigned)s_reads, (unsigned)s_errors);
    } else {
        ESP_LOGI(TAG, "env: no reading yet (reads=%u errors=%u)", (unsigned)s_reads, (unsigned)s_errors);
    }
}
