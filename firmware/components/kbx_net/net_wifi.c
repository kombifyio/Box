/*
 * kombify box - Wi-Fi station with NVS credentials and reconnect backoff.
 */
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "net_wifi.h"

static const char *TAG = "net_wifi";

#define NET_NVS_NAMESPACE "net"
#define NET_NVS_KEY_SSID "wifi_ssid"
#define NET_NVS_KEY_PASS "wifi_pass"

static bool s_wifi_started = false;
static bool s_driver_inited = false;
static bool s_sta_netif_created = false;
static bool s_enabled = true;              /* UI switch; credentials stay */
static volatile bool s_got_ip = false;
static char s_ip[16] = "";
static char s_ssid[33] = "";
static int s_retry = 0;
static esp_timer_handle_t s_reconnect_timer = NULL;
static bool s_sntp_started;

/* Backoff steps in ms; last step repeats forever. */
static const int k_backoff_ms[] = { 1000, 2000, 5000, 10000, 30000 };

static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        /* Intentionally stopped (radio off OR credentials cleared)? Then do
         * NOT reconnect. The disconnect event arrives async AFTER set_enabled(false)/
         * clear_credentials, which already stopped the timer — without this
         * guard it would be armed again here. Both cases are covered by
         * (!s_enabled || empty SSID). */
        if (!s_enabled || !s_ssid[0]) {
            return;
        }
        int idx = s_retry < (int)(sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0]))
                      ? s_retry
                      : (int)(sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0])) - 1;
        s_retry++;
        ESP_LOGW(TAG, "disconnected, reconnect in %d ms (attempt %d)", k_backoff_ms[idx], s_retry);
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)k_backoff_ms[idx] * 1000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_got_ip = true;
        s_retry = 0;
        ESP_LOGI(TAG, "connected: %s @ %s", s_ssid, s_ip);
        /* Account-token expiry and certificate validation need a real clock.
         * Initialization is nonblocking; Cloud auth waits before using TLS. */
        if (!s_sntp_started) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            config.wait_for_sync = false;
            esp_err_t err = esp_netif_sntp_init(&config);
            s_sntp_started = err == ESP_OK;
            if (err != ESP_OK) ESP_LOGW(TAG, "time sync init: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(nvs, NET_NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, NET_NVS_KEY_PASS, pass, &pass_len);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t ensure_wifi_driver(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (s_driver_inited && esp_wifi_get_mode(&mode) == ESP_OK) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_sta_netif_created) {
        if (esp_netif_create_default_wifi_sta() != NULL) {
            s_sta_netif_created = true;
        }
    }
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = reconnect_timer_cb,
            .name = "wifi_reconnect",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "timer");
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi_init: %s int=%u largest=%u",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return err;
    }
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi handler: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ip handler: %s", esp_err_to_name(err));
        return err;
    }
    s_driver_inited = true;
    return ESP_OK;
}

static esp_err_t start_sta(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_ERROR(ensure_wifi_driver(), TAG, "wifi driver");

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = strlen(pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_retry = 0;
    s_got_ip = false;
    s_enabled = true;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode: %s int=%u largest=%u",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        return err;
    }
    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
        }
        return err;
    }
    err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_CONN) {
        /* Driver already running; STA_START will not fire again. */
        s_wifi_started = true;
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
        }
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;
    return ESP_OK;
}

esp_err_t net_wifi_init(void)
{
    esp_err_t err = ensure_wifi_driver();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi driver at boot failed (%s) - CDC wifi can retry",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    char ssid[33] = "";
    char pass[65] = "";
    err = load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err == ESP_OK && ssid[0]) {
        ESP_LOGI(TAG, "credentials found in NVS, connecting to %s", ssid);
        return start_sta(ssid, pass);
    }
    ESP_LOGI(TAG, "no Wi-Fi credentials (%s) - USB-only (provision with KBX wifi <ssid>|<pass>)",
             esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t net_wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!password) {
        password = "";
    }

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open");
    esp_err_t err = nvs_set_str(nvs, NET_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NET_NVS_KEY_PASS, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs write");

    ESP_LOGI(TAG, "credentials saved, connecting to %s", ssid);
    return start_sta(ssid, password);
}

esp_err_t net_wifi_clear_credentials(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open");
    nvs_erase_key(nvs, NET_NVS_KEY_SSID);
    nvs_erase_key(nvs, NET_NVS_KEY_PASS);
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);

    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    s_got_ip = false;
    s_ssid[0] = '\0';
    ESP_LOGI(TAG, "credentials cleared, Wi-Fi stopped");
    return err;
}

bool net_wifi_is_connected(char *ip_out, size_t ip_out_len)
{
    if (ip_out && ip_out_len > 0) {
        strlcpy(ip_out, s_got_ip ? s_ip : "", ip_out_len);
    }
    return s_got_ip;
}

const char *net_wifi_ssid(void)
{
    return s_ssid;
}

esp_err_t net_wifi_set_enabled(bool enabled)
{
    if (enabled == s_enabled) {
        return ESP_OK;
    }
    s_enabled = enabled;

    if (!enabled) {
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }
        if (s_wifi_started) {
            esp_wifi_disconnect();
            esp_wifi_stop();
            s_wifi_started = false;
        }
        s_got_ip = false;
        s_retry = 0;
        ESP_LOGI(TAG, "Wi-Fi radio off (credentials kept)");
        return ESP_OK;
    }

    /* radio on again: connect with stored credentials (config is still set
     * after esp_wifi_stop; without SSID it stays USB-only). */
    if (!s_ssid[0]) {
        ESP_LOGI(TAG, "Wi-Fi radio on, but no credentials (KBX wifi <ssid>|<pass>)");
        return ESP_OK;
    }
    s_retry = 0;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi radio on: start failed: %s", esp_err_to_name(err));
        s_enabled = false;
        return err;
    }
    s_wifi_started = true;
    ESP_LOGI(TAG, "Wi-Fi radio on, connecting to %s", s_ssid);
    return ESP_OK;
}

bool net_wifi_is_enabled(void)
{
    return s_enabled;
}

void net_wifi_release_ram(void)
{
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_driver_inited) {
        (void)esp_wifi_deinit();
        s_driver_inited = false;
    }
    s_got_ip = false;
    ESP_LOGI(TAG, "wifi driver released (OTA RAM)");
}

void net_wifi_log_status(void)
{
    if (!s_ssid[0]) {
        ESP_LOGI(TAG, "status: no credentials (KBX wifi <ssid>|<pass>)");
    } else if (s_got_ip) {
        ESP_LOGI(TAG, "status: connected to %s, IP %s", s_ssid, s_ip);
    } else {
        ESP_LOGI(TAG, "status: configured for %s, not connected (attempt %d)", s_ssid, s_retry);
    }
}
