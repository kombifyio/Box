/*
 * kombify box - battery level (RLCD 4.2: one 18650 Li-ion cell).
 *
 * BAT_ADC_GPIO sees VBAT through a 200K/100K divider (VBAT = 3 x Vadc,
 * schematic). A low-priority task averages 16 calibrated readings every
 * 20 s; the percentage follows a Li-ion discharge curve. With USB present
 * the charger holds the cell near 4.2 V, so the level reads high while it
 * charges; there is no charge-status GPIO on this board.
 */
#include <stdlib.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "board.h"

static const char *TAG = "kbx_bat";

#if CONFIG_KBX_BATTERY

#define BAT_PERIOD_MS 20000
#define BAT_SAMPLES   16

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_channel;
static volatile int s_mv = -1;

/* mV -> % for one Li-ion cell at light load */
static const struct { int mv, pct; } k_curve[] = {
    { 4150, 100 }, { 4100, 95 }, { 4000, 85 }, { 3900, 72 }, { 3800, 57 },
    { 3700, 40 }, { 3600, 22 }, { 3500, 10 }, { 3400, 4 }, { 3300, 0 },
};

static int percent_of(int mv)
{
    if (mv >= k_curve[0].mv) return 100;
    for (size_t i = 1; i < sizeof(k_curve) / sizeof(k_curve[0]); i++) {
        if (mv >= k_curve[i].mv) {
            const int span = k_curve[i - 1].mv - k_curve[i].mv;
            return k_curve[i].pct + (k_curve[i - 1].pct - k_curve[i].pct) * (mv - k_curve[i].mv) / span;
        }
    }
    return 0;
}

static int read_mv(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, s_channel, &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3100 / 4095;   /* uncalibrated: 12 dB range ~3.1 V */
        n++;
    }
    return n ? sum / n * 3 : -1;
}

static void bat_task(void *arg)
{
    (void)arg;
    for (;;) {
        s_mv = read_mv();
        vTaskDelay(pdMS_TO_TICKS(BAT_PERIOD_MS));
    }
}

esp_err_t battery_init(void)
{
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(BAT_ADC_GPIO, &unit, &s_channel);
    if (err != ESP_OK) return err;
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) return err;
    const adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    err = adc_oneshot_config_channel(s_adc, s_channel, &ccfg);
    if (err != ESP_OK) return err;
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = s_channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
    s_mv = read_mv();
    if (xTaskCreate(bat_task, "kbx_bat", 2560, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "battery: %d mV (%d %%), ADC GPIO%d%s", s_mv, battery_percent(), BAT_ADC_GPIO,
             s_cali ? "" : " uncalibrated");
    return ESP_OK;
}

int battery_mv(void) { return s_mv; }

int battery_percent(void)
{
    const int mv = s_mv;
    return mv < 0 ? -1 : percent_of(mv);
}

void battery_log(void)
{
    const int mv = s_mv;
    const char *note = mv < 0 ? "no reading" : mv < 2500 ? "no cell (or a dead one)" : mv < 3300 ? "empty"
                       : mv > 4250 ? "no cell or charger only" : "ok";
    ESP_LOGI(TAG, "battery: %d mV, %d %% (%s)", mv, battery_percent(), note);
}

#else

esp_err_t battery_init(void) { return ESP_ERR_NOT_SUPPORTED; }
int battery_mv(void) { return -1; }
int battery_percent(void) { return -1; }
void battery_log(void) { ESP_LOGI(TAG, "battery: this board has no battery gauge"); }

#endif
