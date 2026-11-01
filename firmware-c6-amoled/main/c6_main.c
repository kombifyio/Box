/*
 * kombify box - Cloud voice satellite on the Waveshare ESP32-C6-Touch-AMOLED-1.8.
 */
#include <string.h>
#include "bsp/esp-bsp.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "c6_audio.h"
#include "c6_console.h"
#include "c6_sd.h"
#include "c6_edge.h"
#include "c6_ui.h"
#include "c6_voice.h"
#include "c6_wake.h"
#include "cloud_auth.h"
#include "net_wifi.h"

static const char *TAG = "kombify_box_c6";

extern const uint8_t kbx_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kbx_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

/* esp_crt_bundle reads 16-bit fields at odd offsets with plain loads. From
 * the flash cache that faults on the C6 (load access fault in
 * esp_crt_ca_cb_callback on the first TLS handshake); internal RAM takes
 * misaligned loads, so the ~16 KB bundle lives there. */
static void crt_bundle_to_ram(void)
{
    size_t size = (size_t)(kbx_crt_bundle_end - kbx_crt_bundle_start);
    uint8_t *ram = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ram) {
        ESP_LOGE(TAG, "no RAM for the certificate bundle (%u B)", (unsigned)size);
        return;
    }
    memcpy(ram, kbx_crt_bundle_start, size);
    ESP_ERROR_CHECK(esp_crt_bundle_set(ram, size));
    ESP_LOGI(TAG, "certificate bundle in RAM (%u B)", (unsigned)size);
}

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "heap after %s: free=%u largest=%u", stage, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "kombify box C6 %s (%s)", app->version, app->project_name);

    /* The expander powers the amplifier on by default; silence it first. */
    ESP_ERROR_CHECK(bsp_i2c_init());
    (void)bsp_audio_poweramp_enable(false);

    crt_bundle_to_ram();
    ESP_ERROR_CHECK(net_wifi_init());
    log_heap("wifi");
    ESP_ERROR_CHECK(cloud_auth_init());
    ESP_ERROR_CHECK(c6_audio_init());
    ESP_ERROR_CHECK(c6_voice_init());
    c6_edge_init();
    ESP_ERROR_CHECK(c6_ui_start());
    ESP_ERROR_CHECK(c6_wake_init());
    c6_sd_init();
    log_heap("ui");
    ESP_ERROR_CHECK(c6_console_start());

    /* Reaching here healthy confirms an OTA-installed image. */
    esp_ota_mark_app_valid_cancel_rollback();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        log_heap("minute");
    }
}
