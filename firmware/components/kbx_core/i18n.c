#include "i18n.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kbx_i18n";

#define I18N_NVS_NAMESPACE "kbx_ui"
#define I18N_NVS_KEY       "locale"

extern const char *const kbx_i18n_EN[I18N__COUNT];
extern const char *const kbx_i18n_DE[I18N__COUNT];
extern const char *const kbx_i18n_ES[I18N__COUNT];
extern const char *const kbx_i18n_ZH_HANS[I18N__COUNT];
extern const char *const kbx_i18n_AR[I18N__COUNT];

extern const lv_font_t kbx_lat_14, kbx_lat_16, kbx_lat_18, kbx_lat_20;
extern const lv_font_t kbx_zh_14, kbx_zh_16, kbx_zh_18, kbx_zh_20;
extern const lv_font_t kbx_ar_14, kbx_ar_16, kbx_ar_18, kbx_ar_20;

/* [locale family][ramp step]. Latin (en/de/es) uses the kbx_lat subset,
 * which falls back to LVGL's built-in Montserrat for ASCII and symbols and
 * adds accents plus the ellipsis that the built-in font lacks. */
static const lv_font_t *const FONT_LAT[] = { &kbx_lat_14, &kbx_lat_16, &kbx_lat_18, &kbx_lat_20 };
static const lv_font_t *const FONT_ZH[] = { &kbx_zh_14, &kbx_zh_16, &kbx_zh_18, &kbx_zh_20 };
static const lv_font_t *const FONT_AR[] = { &kbx_ar_14, &kbx_ar_16, &kbx_ar_18, &kbx_ar_20 };

static volatile i18n_locale_t s_requested = I18N_LOCALE_EN;
static volatile uint32_t s_epoch;

static i18n_locale_t effective(i18n_locale_t l)
{
    return l == I18N_LOCALE_HI ? I18N_LOCALE_EN : l;
}

const char *i18n_locale_tag(i18n_locale_t locale)
{
    switch (locale) {
    case I18N_LOCALE_DE: return "de";
    case I18N_LOCALE_ES: return "es";
    case I18N_LOCALE_ZH_HANS: return "zh-Hans";
    case I18N_LOCALE_AR: return "ar";
    case I18N_LOCALE_HI: return "hi";
    default: return "en";
    }
}

/* Language subtag + script/region check. Traditional Chinese is not shipped
 * (zh-Hant/TW/HK/MO) and resolves to English rather than to the wrong script. */
static i18n_locale_t parse_tag(const char *tag)
{
    char lang[4] = {0};
    size_t n = 0;
    while (tag[n] && tag[n] != '-' && tag[n] != '_' && n < sizeof(lang) - 1) {
        lang[n] = (char)tolower((unsigned char)tag[n]);
        n++;
    }
    const char *rest = tag + n;
    if (strcmp(lang, "de") == 0) return I18N_LOCALE_DE;
    if (strcmp(lang, "es") == 0) return I18N_LOCALE_ES;
    if (strcmp(lang, "ar") == 0) return I18N_LOCALE_AR;
    if (strcmp(lang, "hi") == 0) return I18N_LOCALE_HI;
    if (strcmp(lang, "zh") == 0) {
        if (strcasestr(rest, "hant") || strcasestr(rest, "-tw") || strcasestr(rest, "_tw") ||
            strcasestr(rest, "-hk") || strcasestr(rest, "_hk") || strcasestr(rest, "-mo") ||
            strcasestr(rest, "_mo")) {
            return I18N_LOCALE_EN;
        }
        return I18N_LOCALE_ZH_HANS;
    }
    return I18N_LOCALE_EN;
}

static void nvs_write_tag(const char *tag)
{
    nvs_handle_t h;
    if (nvs_open(I18N_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "locale not persisted (nvs open failed)");
        return;
    }
    nvs_set_str(h, I18N_NVS_KEY, tag);
    nvs_commit(h);
    nvs_close(h);
}

void i18n_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "nvs unavailable (%s) - English", esp_err_to_name(err));
        return;
    }
    nvs_handle_t h;
    if (nvs_open(I18N_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return; /* never set: English */
    }
    char tag[16] = {0};
    size_t len = sizeof(tag);
    if (nvs_get_str(h, I18N_NVS_KEY, tag, &len) == ESP_OK && tag[0]) {
        s_requested = parse_tag(tag);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "locale %s (rendering %s)", i18n_locale_tag(s_requested),
             i18n_locale_tag(effective(s_requested)));
}

esp_err_t i18n_set_locale_tag(const char *tag)
{
    if (!tag || !tag[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    i18n_locale_t next = parse_tag(tag);
    if (next == s_requested) {
        return ESP_OK; /* session-create re-sends the tag: no flash wear */
    }
    i18n_locale_t before = effective(s_requested);
    s_requested = next;
    nvs_write_tag(i18n_locale_tag(next));
    if (effective(next) != before) {
        s_epoch++;
    }
    ESP_LOGI(TAG, "locale set to %s (rendering %s)", i18n_locale_tag(next),
             i18n_locale_tag(effective(next)));
    if (next == I18N_LOCALE_HI) {
        ESP_LOGW(TAG, "hi: Devanagari cannot be shaped by LVGL - display stays English");
    }
    return ESP_OK;
}

i18n_locale_t i18n_locale(void) { return effective(s_requested); }
i18n_locale_t i18n_requested_locale(void) { return s_requested; }
uint32_t i18n_epoch(void) { return s_epoch; }

const char *i18n_str(i18n_id_t id)
{
    if ((unsigned)id >= I18N__COUNT) {
        return "";
    }
    const char *s = NULL;
    switch (effective(s_requested)) {
    case I18N_LOCALE_DE: s = kbx_i18n_DE[id]; break;
    case I18N_LOCALE_ES: s = kbx_i18n_ES[id]; break;
    case I18N_LOCALE_ZH_HANS: s = kbx_i18n_ZH_HANS[id]; break;
    case I18N_LOCALE_AR: s = kbx_i18n_AR[id]; break;
    default: break;
    }
    return s ? s : kbx_i18n_EN[id];
}

const lv_font_t *i18n_font(i18n_font_size_t size)
{
    unsigned i = (unsigned)size;
    if (i > (unsigned)I18N_FONT_20) i = (unsigned)I18N_FONT_16;
    switch (effective(s_requested)) {
    case I18N_LOCALE_ZH_HANS: return FONT_ZH[i];
    case I18N_LOCALE_AR: return FONT_AR[i];
    default: return FONT_LAT[i];
    }
}

void i18n_style_label(lv_obj_t *label, i18n_font_size_t size)
{
    lv_obj_set_style_text_font(label, i18n_font(size), 0);
    lv_obj_set_style_base_dir(label,
                              effective(s_requested) == I18N_LOCALE_AR ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
}

void i18n_log_status(void)
{
    ESP_LOGI(TAG, "locale=%s rendering=%s (supported: en de es zh-Hans ar; hi renders as en)",
             i18n_locale_tag(s_requested), i18n_locale_tag(effective(s_requested)));
}
