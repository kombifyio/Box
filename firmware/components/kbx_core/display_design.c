/* kombify box - display design choice. Contract: display_design.h. */
#include "display_design.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "kbx_design";

#define NVS_NS  "kbx_ui"
#define NVS_KEY "design"      /* u8: 0 classic, 1 watch; absent = the board's default */

static kbx_design_t s_default = KBX_DESIGN_CLASSIC;
static kbx_design_t s_running = KBX_DESIGN_CLASSIC;
static kbx_design_t s_stored = KBX_DESIGN_CLASSIC;
static bool s_chosen;                 /* the account or the console picked one */

void kbx_design_init(kbx_design_t board_default)
{
    static bool s_inited;
    if (s_inited) return;          /* the running design is fixed for this boot */
    s_inited = true;
    s_default = board_default;
    s_stored = board_default;
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_KEY, &v) == ESP_OK && v <= KBX_DESIGN_WATCH) {
            s_stored = (kbx_design_t)v;
            s_chosen = true;
        }
        nvs_close(h);
    }
    s_running = s_stored;
    ESP_LOGI(TAG, "design: %s (default %s)", kbx_design_id(s_running), kbx_design_id(s_default));
}

kbx_design_t kbx_design(void) { return s_running; }

kbx_design_t kbx_design_stored(void) { return s_stored; }

esp_err_t kbx_design_set(kbx_design_t design)
{
    if (design != KBX_DESIGN_CLASSIC && design != KBX_DESIGN_WATCH) return ESP_ERR_INVALID_ARG;
    if (design == s_stored && s_chosen) return ESP_OK;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY, (uint8_t)design);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "design not stored: %s", esp_err_to_name(err));
        return err;
    }
    s_stored = design;
    s_chosen = true;
    ESP_LOGI(TAG, "design %s stored%s", kbx_design_id(design),
             design != s_running ? ", restart pending" : "");
    return ESP_OK;
}

esp_err_t kbx_design_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    s_stored = s_default;
    s_chosen = false;
    ESP_LOGI(TAG, "design cleared, the board default %s applies%s", kbx_design_id(s_default),
             s_default != s_running ? ", restart pending" : "");
    return ESP_OK;
}

bool kbx_design_chosen(void) { return s_chosen; }

bool kbx_design_restart_needed(void) { return s_stored != s_running; }

const char *kbx_design_id(kbx_design_t design)
{
    return design == KBX_DESIGN_WATCH ? "watch" : "classic";
}

bool kbx_design_from_id(const char *id, kbx_design_t *out)
{
    if (!id) return false;
    if (!strcmp(id, "classic")) {
        if (out) *out = KBX_DESIGN_CLASSIC;
        return true;
    }
    if (!strcmp(id, "watch")) {
        if (out) *out = KBX_DESIGN_WATCH;
        return true;
    }
    return false;
}
