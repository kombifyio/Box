/*
 * kombify box - OTA apply (CDC stream + HTTP pull) and rollback health gate.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "tusb.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"
#include "esp32s3/rom/usb/usb_persist.h"
#endif

#include "box_status.h"
#include "fw_ota.h"
#include "fw_update_manifest.h"
#include "net_wifi.h"
#include "net_ws.h"

static const char *TAG = "kbx_ota";


#define OTA_URL_MAX 512
#define OTA_CDC_CHUNK 1024
#define OTA_CDC_IDLE_MS 20000
#define OTA_PROGRESS_STEP 262144

typedef enum {
    OTA_PHASE_IDLE = 0,
    OTA_PHASE_PREPARE,
    OTA_PHASE_TRANSFER,
    OTA_PHASE_FINISH,
    OTA_PHASE_REBOOT,
    OTA_PHASE_ERR,
} ota_phase_t;

static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_lock;
static volatile ota_phase_t s_phase = OTA_PHASE_IDLE;
static volatile bool s_cdc_active = false;
static volatile bool s_busy = false;
static char s_detail[96] = "idle";
static char s_url[OTA_URL_MAX];
static size_t s_cdc_size;
static bool s_cdc_have_sha;
static uint8_t s_cdc_want_sha[32];
static portMUX_TYPE s_update_mux = portMUX_INITIALIZER_UNLOCKED;
static fw_ota_update_status_t s_update;
static fw_update_manifest_t s_manifest;
static bool s_manifest_valid;
static char s_want_version[32];   /* pinned release of the running check, "" = latest */

static void update_status(fw_ota_update_state_t state, const char *detail, unsigned percent)
{
    portENTER_CRITICAL(&s_update_mux);
    s_update.state = state;
    s_update.progress_percent = percent;
    snprintf(s_update.detail, sizeof(s_update.detail), "%s", detail ? detail : "");
    portEXIT_CRITICAL(&s_update_mux);
}

void fw_ota_get_update_status(fw_ota_update_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_update_mux);
    *out = s_update;
    portEXIT_CRITICAL(&s_update_mux);
}


static const char *phase_name(ota_phase_t p)
{
    switch (p) {
    case OTA_PHASE_PREPARE: return "prepare";
    case OTA_PHASE_TRANSFER: return "transfer";
    case OTA_PHASE_FINISH: return "finish";
    case OTA_PHASE_REBOOT: return "reboot";
    case OTA_PHASE_ERR: return "err";
    case OTA_PHASE_IDLE:
    default: return "idle";
    }
}

static const char *img_state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
    default: return "?";
    }
}

static void set_phase(ota_phase_t phase, const char *detail)
{
    s_phase = phase;
    if (detail) {
        strncpy(s_detail, detail, sizeof(s_detail) - 1);
        s_detail[sizeof(s_detail) - 1] = '\0';
    }
}

static bool parse_sha256_hex(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        if (sscanf(hex + (i * 2), "%2x", &v) != 1) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

static void sha_to_hex(const uint8_t sha[32], char *out, size_t out_len)
{
    if (out_len < 65) {
        if (out_len) {
            out[0] = '\0';
        }
        return;
    }
    for (int i = 0; i < 32; i++) {
        sprintf(out + (i * 2), "%02x", sha[i]);
    }
    out[64] = '\0';
}

static void hex_bytes(const uint8_t *in, size_t n, char *out, size_t out_len)
{
    size_t max = (out_len - 1) / 2;
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        sprintf(out + (i * 2), "%02x", in[i]);
    }
    out[n * 2] = '\0';
}

static void confirm_running_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running) {
        (void)esp_ota_get_state_partition(running, &st);
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "mark-valid (was pending) %s",
             err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static bool pause_voice(bool release_wifi)
{
    net_ws_suspend();
    box_status_set(KBX_HOST_THINKING);
    if (release_wifi) {
        net_wifi_release_ram();
        vTaskDelay(pdMS_TO_TICKS(1500));
        return true;
    }
    /* Session creation and DELETE can be in flight after the UI pauses it.
     * Wait for the lifecycle's actual drain acknowledgement before new TLS. */
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (!net_ws_is_suspended_idle()) {
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

static void reboot_into_new_image(void)
{
    update_status(FW_OTA_UPDATE_REBOOTING, "reboot", 100);
    set_phase(OTA_PHASE_REBOOT, "reboot");
    ESP_LOGI(TAG, "complete - rebooting into new slot");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static void finish_cdc_fail(esp_ota_handle_t *handle, const char *why)
{
    s_cdc_active = false;
    if (handle && *handle) {
        (void)esp_ota_abort(*handle);
        *handle = 0;
    }
    set_phase(OTA_PHASE_ERR, why);
    ESP_LOGE(TAG, "err %s", why);
    s_busy = false;
}

static void cdc_task(void *arg)
{
    (void)arg;
    const size_t image_size = s_cdc_size;
    const bool check_sha = s_cdc_have_sha;
    uint8_t want_sha[32];
    if (check_sha) {
        memcpy(want_sha, s_cdc_want_sha, sizeof(want_sha));
    }

    pause_voice(true);
    confirm_running_if_pending();
    set_phase(OTA_PHASE_PREPARE, "erase");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        finish_cdc_fail(NULL, "no-update-slot");
        vTaskDelete(NULL);
        return;
    }
    if (image_size < 1024 || image_size > part->size) {
        finish_cdc_fail(NULL, "size");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "begin size=%u slot=%s offset=0x%lx",
             (unsigned)image_size, part->label, (unsigned long)part->address);

    /* Sequential writes erase flash in small steps during esp_ota_write.
     * A full-size erase here stalls CPU0 long enough for the idle-task
     * watchdog to reset the chip (observed: ota-begin logged, then reboot). */
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        char why[48];
        snprintf(why, sizeof(why), "%s", esp_err_to_name(err));
        finish_cdc_fail(&handle, why);
        vTaskDelete(NULL);
        return;
    }

    psa_crypto_init(); /* idempotent; mbedTLS 4 exposes hashes only via PSA */
    psa_hash_operation_t sha_ctx = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&sha_ctx, PSA_ALG_SHA_256);

    uint8_t *chunk = heap_caps_malloc(OTA_CDC_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        chunk = malloc(OTA_CDC_CHUNK);
    }
    if (!chunk) {
        finish_cdc_fail(&handle, "no-mem");
        psa_hash_abort(&sha_ctx);
        vTaskDelete(NULL);
        return;
    }

    s_cdc_active = true;
    set_phase(OTA_PHASE_TRANSFER, "recv");
    ESP_LOGI(TAG, "recv"); /* host waits for this exact token before sending */

    size_t got = 0;
    size_t last_log = 0;
    int64_t last_byte_us = esp_timer_get_time();
    while (got < image_size) {
        uint32_t avail = tud_cdc_available();
        if (avail == 0) {
            if ((esp_timer_get_time() - last_byte_us) / 1000 > OTA_CDC_IDLE_MS) {
                free(chunk);
                psa_hash_abort(&sha_ctx);
                finish_cdc_fail(&handle, "timeout");
                vTaskDelete(NULL);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t want = image_size - got;
        if (want > OTA_CDC_CHUNK) {
            want = OTA_CDC_CHUNK;
        }
        if (want > avail) {
            want = avail;
        }
        uint32_t n = tud_cdc_read(chunk, want);
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        err = esp_ota_write(handle, chunk, n);
        if (err != ESP_OK) {
            free(chunk);
            psa_hash_abort(&sha_ctx);
            finish_cdc_fail(&handle, esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
        psa_hash_update(&sha_ctx, chunk, n);
        got += n;
        last_byte_us = esp_timer_get_time();
        if (got - last_log >= OTA_PROGRESS_STEP || got == image_size) {
            unsigned pct = (unsigned)((got * 100ULL) / image_size);
            ESP_LOGI(TAG, "progress %u%% %u/%u", pct, (unsigned)got, (unsigned)image_size);
            last_log = got;
        }
    }
    s_cdc_active = false;
    free(chunk);

    uint8_t got_sha[32];
    size_t got_sha_len = 0;
    psa_hash_finish(&sha_ctx, got_sha, sizeof(got_sha), &got_sha_len);
    psa_hash_abort(&sha_ctx);
    char sha_hex[65];
    sha_to_hex(got_sha, sha_hex, sizeof(sha_hex));

    if (check_sha && memcmp(got_sha, want_sha, 32) != 0) {
        finish_cdc_fail(&handle, "sha256");
        ESP_LOGE(TAG, "sha256 mismatch got=%s", sha_hex);
        vTaskDelete(NULL);
        return;
    }

    set_phase(OTA_PHASE_FINISH, "validate");
    err = esp_ota_end(handle);
    handle = 0;
    if (err != ESP_OK) {
        finish_cdc_fail(&handle, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        finish_cdc_fail(&handle, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "written slot=%s sha256=%s", part->label, sha_hex);
    reboot_into_new_image();
}

/* GitHub's release download redirect is public and credential-free. Never
 * forward a device token, follow HTTP, or leave these two publisher hosts. */
typedef struct { char location[2048]; } release_http_t;

static esp_err_t release_http_event(esp_http_client_event_t *event)
{
    release_http_t *state = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        strcasecmp(event->header_key, "Location") == 0) {
        size_t n = strlen(event->header_value);
        if (n >= sizeof(state->location)) return ESP_ERR_INVALID_SIZE;
        memcpy(state->location, event->header_value, n + 1);
    }
    return ESP_OK;
}

static esp_http_client_handle_t release_open(const char *url, release_http_t *state)
{
    if (!fw_update_release_url_allowed(url)) return NULL;
    esp_http_client_config_t config = {
        .url = url, .timeout_ms = 30000, .buffer_size = 4096,
        /* GitHub's signed asset query exceeds the default request-line buffer. */
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = release_http_event, .user_data = state,
        .user_agent = "kombify-box-ota/1",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    for (unsigned redirects = 0; redirects <= 3; ++redirects) {
        state->location[0] = 0;
        if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0) break;
        int status = esp_http_client_get_status_code(client);
        if (status == 200) return client;
        if ((status != 301 && status != 302 && status != 303 && status != 307 && status != 308) ||
            redirects == 3 || !fw_update_release_url_allowed(state->location)) break;
        esp_http_client_close(client);
        if (esp_http_client_set_url(client, state->location) != ESP_OK) break;
    }
    esp_http_client_cleanup(client);
    return NULL;
}

static void release_voice(void)
{
    net_ws_resume();
}

static void update_fail(const char *why)
{
    set_phase(OTA_PHASE_ERR, why);
    update_status(FW_OTA_UPDATE_ERROR, why, 0);
    release_voice();
    s_busy = false;
    ESP_LOGE(TAG, "update failed: %s", why);
}

static void http_task(void *arg)
{
    (void)arg;
    esp_http_client_handle_t client = NULL;
    esp_ota_handle_t handle = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    uint8_t *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    release_http_t *redirect = heap_caps_calloc(1, sizeof(*redirect), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char *failure = "no-mem";
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || s_manifest.size > part->size) { failure = "image-size"; goto fail; }
    if (!chunk || !redirect) goto fail;
    client = release_open(s_url, redirect);
    failure = "download-unavailable";
    if (!client) goto fail;
    int64_t length = esp_http_client_get_content_length(client);
    if (length >= 0 && (uint64_t)length != s_manifest.size) { failure = "image-size"; goto fail; }
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        failure = "sha256-init"; goto fail;
    }
    confirm_running_if_pending();
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) { failure = "ota-begin"; goto fail; }
    set_phase(OTA_PHASE_TRANSFER, "https");
    size_t got = 0;
    while (got < s_manifest.size) {
        size_t want = s_manifest.size - got;
        if (want > 4096) want = 4096;
        int n = esp_http_client_read(client, (char *)chunk, want);
        if (n <= 0) { failure = "incomplete-image"; goto fail; }
        if (psa_hash_update(&hash, chunk, n) != PSA_SUCCESS) { failure = "sha256-update"; goto fail; }
        if (esp_ota_write(handle, chunk, n) != ESP_OK) { failure = "flash-write"; goto fail; }
        got += (size_t)n;
        update_status(FW_OTA_UPDATE_INSTALLING, "download", (unsigned)(got * 100ULL / s_manifest.size));
    }
    /* Reject extra bytes and truncated HTTP framing, including chunked bodies. */
    if (esp_http_client_read(client, (char *)chunk, 1) != 0 ||
        !esp_http_client_is_complete_data_received(client)) { failure = "image-size"; goto fail; }
    uint8_t digest[32];
    size_t digest_size = 0;
    if (psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS ||
        digest_size != sizeof(digest) || memcmp(digest, s_manifest.sha256, sizeof(digest))) {
        failure = "sha256-mismatch"; goto fail;
    }
    set_phase(OTA_PHASE_FINISH, "validate");
    esp_err_t end = esp_ota_end(handle);
    handle = 0; /* esp_ota_end releases its handle even on validation failure */
    if (end != ESP_OK) { failure = "invalid-image"; goto fail; }
    esp_app_desc_t incoming;
    if (esp_ota_get_partition_description(part, &incoming) != ESP_OK ||
        strcmp(incoming.project_name, "kombify_box") ||
        strcmp(incoming.version, s_manifest.version) ||
        memcmp(incoming.app_elf_sha256, s_manifest.elf_sha256, 32)) {
        failure = "image-identity"; goto fail;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) { failure = "boot-selection"; goto fail; }
    esp_http_client_cleanup(client);
    psa_hash_abort(&hash);
    free(chunk); free(redirect);
    reboot_into_new_image();
    vTaskDelete(NULL);
    return;
fail:
    if (handle) esp_ota_abort(handle);
    if (client) esp_http_client_cleanup(client);
    psa_hash_abort(&hash);
    free(chunk); free(redirect);
    update_fail(failure);
    vTaskDelete(NULL);
}

static bool spawn_ota_task(TaskFunction_t fn, const char *name)
{
    /* TLS needs the same 16 KiB budget as cloud_auth. Once voice closes the
     * internal heap can carry it; never retry TLS on a smaller stack. */
    if (fn == http_task) {
        return xTaskCreate(fn, name, 16384, NULL, 5, NULL) == pdPASS;
    }
    /* Internal RAM only: flash ops unmap PSRAM stacks (WDT after ota-begin).
     * Idle heap_int is ~13 KB, so 10 KB often fails to allocate. CDC needs
     * little stack (chunk is PSRAM); HTTP retries a larger size first. */
    const uint32_t stacks[] = { 6144, 5120, 4096 };
    for (size_t i = 0; i < sizeof(stacks) / sizeof(stacks[0]); i++) {
        if (xTaskCreate(fn, name, stacks[i], NULL, 5, NULL) == pdPASS) {
            ESP_LOGI(TAG, "task %s stack=%u heap_int=%u", name,
                     (unsigned)stacks[i],
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            return true;
        }
    }
    ESP_LOGE(TAG, "task %s create failed heap_int=%u largest=%u", name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return false;
}

static SemaphoreHandle_t ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
    }
    return s_lock;
}

static bool claim_busy(void)
{
    if (!ensure_lock()) {
        ESP_LOGW(TAG, "lock missing");
        return false;
    }
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_busy) {
        s_busy = true;
        set_phase(OTA_PHASE_PREPARE, "queued");
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* ---------------------------------------------------------- LAN push */

static esp_ota_handle_t s_stream_handle;
static const esp_partition_t *s_stream_part;
static psa_hash_operation_t s_stream_sha;
static uint8_t s_stream_want[32];
static size_t s_stream_size, s_stream_got, s_stream_logged;

esp_err_t fw_ota_stream_begin(size_t image_size, const uint8_t want_sha256[32])
{
    if (!want_sha256) return ESP_ERR_INVALID_ARG;
    if (!claim_busy()) {
        ESP_LOGW(TAG, "busy (%s)", s_detail);
        return ESP_ERR_INVALID_STATE;
    }
    if (!pause_voice(false)) {
        fw_ota_stream_abort("voice-busy");
        return ESP_ERR_TIMEOUT;
    }
    confirm_running_if_pending();
    s_stream_part = esp_ota_get_next_update_partition(NULL);
    if (!s_stream_part || image_size < 1024 || image_size > s_stream_part->size) {
        fw_ota_stream_abort(s_stream_part ? "size" : "no-update-slot");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_begin(s_stream_part, OTA_WITH_SEQUENTIAL_WRITES, &s_stream_handle);
    if (err != ESP_OK) {
        fw_ota_stream_abort(esp_err_to_name(err));
        return err;
    }
    s_stream_sha = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&s_stream_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fw_ota_stream_abort("sha256-init");
        return ESP_FAIL;
    }
    memcpy(s_stream_want, want_sha256, sizeof(s_stream_want));
    s_stream_size = image_size;
    s_stream_got = s_stream_logged = 0;
    set_phase(OTA_PHASE_TRANSFER, "lan");
    update_status(FW_OTA_UPDATE_INSTALLING, "lan", 0);
    ESP_LOGI(TAG, "lan begin size=%u slot=%s", (unsigned)image_size, s_stream_part->label);
    return ESP_OK;
}

esp_err_t fw_ota_stream_write(const void *data, size_t len)
{
    if (!s_stream_handle || s_stream_got + len > s_stream_size) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_write(s_stream_handle, data, len);
    if (err != ESP_OK) return err;
    if (psa_hash_update(&s_stream_sha, data, len) != PSA_SUCCESS) return ESP_FAIL;
    s_stream_got += len;
    if (s_stream_got - s_stream_logged >= OTA_PROGRESS_STEP || s_stream_got == s_stream_size) {
        unsigned pct = (unsigned)((s_stream_got * 100ULL) / s_stream_size);
        update_status(FW_OTA_UPDATE_INSTALLING, "lan", pct);
        ESP_LOGI(TAG, "progress %u%% %u/%u", pct, (unsigned)s_stream_got, (unsigned)s_stream_size);
        s_stream_logged = s_stream_got;
    }
    return ESP_OK;
}

esp_err_t fw_ota_stream_finish(void)
{
    if (!s_stream_handle || s_stream_got != s_stream_size) {
        fw_ota_stream_abort("short");
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t got[32];
    size_t got_len = 0;
    if (psa_hash_finish(&s_stream_sha, got, sizeof(got), &got_len) != PSA_SUCCESS ||
        got_len != sizeof(got) || memcmp(got, s_stream_want, sizeof(got))) {
        fw_ota_stream_abort("sha256");
        return ESP_ERR_INVALID_CRC;
    }
    set_phase(OTA_PHASE_FINISH, "validate");
    esp_err_t err = esp_ota_end(s_stream_handle);
    s_stream_handle = 0;
    if (err == ESP_OK) err = esp_ota_set_boot_partition(s_stream_part);
    if (err != ESP_OK) {
        fw_ota_stream_abort(esp_err_to_name(err));
        return err;
    }
    char sha_hex[65];
    sha_to_hex(got, sha_hex, sizeof(sha_hex));
    ESP_LOGI(TAG, "written slot=%s sha256=%s (lan)", s_stream_part->label, sha_hex);
    return ESP_OK;
}

void fw_ota_stream_abort(const char *why)
{
    if (s_stream_handle) {
        (void)esp_ota_abort(s_stream_handle);
        s_stream_handle = 0;
    }
    psa_hash_abort(&s_stream_sha);
    set_phase(OTA_PHASE_ERR, why ? why : "abort");
    update_status(FW_OTA_UPDATE_ERROR, why ? why : "abort", 0);
    ESP_LOGE(TAG, "lan err %s", why ? why : "abort");
    net_ws_resume();
    s_busy = false;
}

void fw_ota_stream_reboot(void)
{
    reboot_into_new_image();
}

const char *fw_ota_reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "UNKNOWN";
    }
}

bool fw_ota_reset_was_crash(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

void fw_ota_log_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running) {
        (void)esp_ota_get_state_partition(running, &st);
    }
    char sha[17];
    hex_bytes(app->app_elf_sha256, 8, sha, sizeof(sha));
    ESP_LOGI(TAG, "version: project=%s version=%s sha=%s idf=%s compiled=%s %s",
             app->project_name, app->version, sha, app->idf_ver, app->date, app->time);
    ESP_LOGI(TAG, "slot: running=%s@0x%lx boot=%s next=%s state=%s",
             running ? running->label : "?",
             running ? (unsigned long)running->address : 0,
             boot ? boot->label : "?",
             next ? next->label : "?",
             img_state_name(st));
    ESP_LOGI(TAG, "boot: reset=%s (%d) uptime=%lld s",
             fw_ota_reset_reason_name(), (int)esp_reset_reason(),
             (long long)(esp_timer_get_time() / 1000000));
}

void fw_ota_log_status(void)
{
    ESP_LOGI(TAG, "status: phase=%s busy=%d cdc=%d detail=%s heap_int=%u",
             phase_name(s_phase), (int)s_busy, (int)s_cdc_active, s_detail,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    fw_ota_log_version();
}

static void check_task(void *arg)
{
    (void)arg;
    if (!pause_voice(false)) {
        update_fail("voice-busy");
        vTaskDeleteWithCaps(NULL);
        return;
    }
    char *json = heap_caps_calloc(1, 2050, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    release_http_t *redirect = heap_caps_calloc(1, sizeof(*redirect), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_handle_t client = NULL;
    const char *failure = "no-mem";
    if (!json || !redirect) goto done;
    char manifest_url[192];
    failure = "invalid-version";
    if (!fw_update_manifest_version_url(s_want_version, manifest_url, sizeof(manifest_url))) goto done;
    client = release_open(manifest_url, redirect);
    failure = "manifest-unavailable";
    if (!client) goto done;
    int n = esp_http_client_read_response(client, json, 2049);
    failure = "invalid-manifest";
    if (n <= 0 || n > 2048 || !esp_http_client_is_complete_data_received(client) ||
        memchr(json, 0, (size_t)n) || !fw_update_manifest_parse(json, &s_manifest)) goto done;
    failure = "version-mismatch";
    if (s_want_version[0] && strcmp(s_want_version, s_manifest.version) != 0) goto done;
    s_manifest_valid = true;
    portENTER_CRITICAL(&s_update_mux);
    snprintf(s_update.version, sizeof(s_update.version), "%s", s_manifest.version);
    portEXIT_CRITICAL(&s_update_mux);
    bool current = !memcmp(esp_app_get_description()->app_elf_sha256, s_manifest.elf_sha256, 32);
    update_status(current ? FW_OTA_UPDATE_CURRENT : FW_OTA_UPDATE_AVAILABLE,
                  current ? "current" : "available", 0);
    set_phase(OTA_PHASE_IDLE, current ? "current" : "available");
    failure = NULL;
done:
    if (client) esp_http_client_cleanup(client);
    free(json); free(redirect);
    if (failure) update_fail(failure);
    else { release_voice(); s_busy = false; }
    vTaskDeleteWithCaps(NULL);
}

static void install_starter(void *arg)
{
    (void)arg;
    if (!pause_voice(false)) {
        update_fail("voice-busy");
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (!spawn_ota_task(http_task, "kbx_ota_http")) update_fail("task-memory");
    vTaskDeleteWithCaps(NULL);
}

bool fw_ota_is_busy(void)
{
    return s_busy || s_phase == OTA_PHASE_REBOOT;
}

esp_err_t fw_ota_check_update(void)
{
    return fw_ota_check_update_version(NULL);
}

esp_err_t fw_ota_check_update_version(const char *version)
{
    if (version && version[0] && !fw_update_version_valid(version)) {
        update_status(FW_OTA_UPDATE_ERROR, "invalid-version", 0);
        return ESP_ERR_INVALID_ARG;
    }
    if (!net_wifi_is_connected(NULL, 0)) {
        update_status(FW_OTA_UPDATE_ERROR, "wifi-required", 0);
        return ESP_ERR_INVALID_STATE;
    }
    if (!claim_busy()) return ESP_ERR_INVALID_STATE;
    s_manifest_valid = false;
    snprintf(s_want_version, sizeof(s_want_version), "%s", version ? version : "");
    update_status(FW_OTA_UPDATE_CHECKING, "checking", 0);
    if (xTaskCreateWithCaps(check_task, "kbx_update", 16384, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        update_fail("task-memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fw_ota_install_update(void)
{
    if (!net_wifi_is_connected(NULL, 0) || !claim_busy()) return ESP_ERR_INVALID_STATE;
    if (!s_manifest_valid || !fw_update_manifest_url(&s_manifest, s_url, sizeof(s_url)) ||
        !memcmp(esp_app_get_description()->app_elf_sha256, s_manifest.elf_sha256, 32)) {
        s_busy = false;
        return ESP_ERR_INVALID_STATE;
    }
    update_status(FW_OTA_UPDATE_INSTALLING, "preparing", 0);
    if (xTaskCreateWithCaps(install_starter, "kbx_ota_start", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        update_fail("task-memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fw_ota_start_http(const char *url)
{
    char expected[OTA_URL_MAX];
    if (!s_manifest_valid || !fw_update_manifest_url(&s_manifest, expected, sizeof(expected)) ||
        !url || strcmp(url, expected)) return ESP_ERR_INVALID_ARG;
    return fw_ota_install_update();
}

esp_err_t fw_ota_begin_cdc(size_t image_size, const char *sha256_hex)
{
    if (image_size < 1024) {
        ESP_LOGW(TAG, "cdc size too small");
        return ESP_ERR_INVALID_ARG;
    }
    s_cdc_have_sha = false;
    if (sha256_hex && *sha256_hex) {
        if (!parse_sha256_hex(sha256_hex, s_cdc_want_sha)) {
            ESP_LOGW(TAG, "cdc sha256 must be 64 hex chars");
            return ESP_ERR_INVALID_ARG;
        }
        s_cdc_have_sha = true;
    }
    if (!claim_busy()) {
        ESP_LOGW(TAG, "busy (%s)", s_detail);
        return ESP_ERR_INVALID_STATE;
    }
    s_cdc_size = image_size;
    pause_voice(true);
    if (!spawn_ota_task(cdc_task, "kbx_ota_cdc")) {
        s_busy = false;
        set_phase(OTA_PHASE_ERR, "task");
        ESP_LOGE(TAG, "cdc task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool fw_ota_cdc_active(void)
{
    return s_cdc_active;
}

void fw_ota_reboot_to_download(void)
{
    ESP_LOGW(TAG, "rebooting into serial download mode");
    vTaskDelay(pdMS_TO_TICKS(200));
    /* Do not persist TinyUSB: ROM download must re-enumerate as PID 1001. */
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

void fw_ota_reboot(void)
{
    if (s_busy) {
        ESP_LOGW(TAG, "restart refused while firmware update is active");
        return;
    }
    ESP_LOGI(TAG, "restarting current firmware");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

esp_err_t fw_ota_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running) {
        (void)esp_ota_get_state_partition(running, &st);
    }
    fw_ota_log_version();
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "health gate idle (state=%s)", img_state_name(st));
        return ESP_OK;
    }
    /* Standalone speaker: app_main only reaches here after display+audio.
     * Waiting for USB or Wi-Fi restarted a healthy box every 60 s whenever
     * no PC was plugged in and STA had no IP. */
    confirm_running_if_pending();
    return ESP_OK;
}
