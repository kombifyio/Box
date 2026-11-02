// kombify box - hardware board probe (board_identity.h).
//
// Both S3 boards carry the same codec pair and RTC (ES8311 0x18, ES7210 0x40,
// PCF85063 0x51), but on different system I2C pins, and each has a device the
// other lacks: the 1.85C its TCA9554 expander (0x20) and CST816S touch (0x15)
// on SDA11/SCL10, the RLCD 4.2 its SHTC3 (0x70) on SDA13/SCL14. A pin pair
// matches a board when one of its own devices and at least one more answer
// and two addresses no board uses stay silent (a stuck or floating line ACKs
// everything). The compiled board's pair is probed first; another board's
// pins are touched only when the compiled pair did not match, which on a
// correctly flashed box never happens.
#include "board_identity.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kbx_board";

#define NVS_NS "kbx_board"
#define NVS_KEY_HW "hw"
#define NVS_KEY_REJECTED "rejected"
#define PROBE_TIMEOUT_MS 10

typedef struct {
    const char *id;           /* = CONFIG_KBX_BOARD_ID of that board's image */
    const char *usb_serial;   /* = CONFIG_UAC_TUSB_SERIAL_NUM of that image */
    gpio_num_t sda, scl;
    gpio_num_t power_latch;   /* GPIO_NUM_NC: no latch */
    uint8_t own[2];           /* devices only this board has (0 = unused) */
    uint8_t shared[3];        /* devices both boards have */
} board_desc_t;

#define B185C_SDA GPIO_NUM_11
#define B185C_SCL GPIO_NUM_10
#define RLCD42_SDA GPIO_NUM_13
#define RLCD42_SCL GPIO_NUM_14

static const board_desc_t k_boards[] = {
    { "waveshare-esp32s3-touch-lcd-1.85", "KBX-0001", B185C_SDA, B185C_SCL, GPIO_NUM_7,
      { 0x20, 0x15 }, { 0x18, 0x40, 0x51 } },
    { "waveshare-esp32s3-rlcd-4.2", "KBX-RLCD42", RLCD42_SDA, RLCD42_SCL, GPIO_NUM_NC,
      { 0x70, 0 }, { 0x18, 0x40, 0x51 } },
};
#define BOARD_COUNT (sizeof(k_boards) / sizeof(k_boards[0]))

/* The table above must describe the same pins as the board profile. */
#if CONFIG_KBX_BOARD_WAVESHARE_RLCD42
#define COMPILED_INDEX 1
_Static_assert(SYS_I2C_SDA == RLCD42_SDA && SYS_I2C_SCL == RLCD42_SCL, "RLCD 4.2 probe pins differ from board_rlcd42.h");
#else
#define COMPILED_INDEX 0
_Static_assert(SYS_I2C_SDA == B185C_SDA && SYS_I2C_SCL == B185C_SCL, "1.85C probe pins differ from board_185c.h");
_Static_assert(PWR_CONTROL_GPIO == GPIO_NUM_7, "1.85C power latch differs from board_185c.h");
#endif

static bool s_probed;
static int s_hw = -1;                 /* index into k_boards, -1 = unknown */
static const char *s_source = "none";
static char s_detail[160] = "not probed";
static char s_rejected[80];

/* 1 = this board answered, 0 = it did not. */
static int probe_pair(const board_desc_t *b, char *out, size_t cap)
{
    int n = snprintf(out, cap, "SDA%d/SCL%d:", (int)b->sda, (int)b->scl);
    if (n < 0 || (size_t)n >= cap) n = 0;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = b->sda,
        .scl_io_num = b->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    int verdict = 0;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        snprintf(out + n, cap - (size_t)n, " bus-error");
        goto release;
    }
    static const uint8_t controls[] = { 0x0B, 0x2C };
    for (size_t i = 0; i < sizeof(controls); i++) {
        if (i2c_master_probe(bus, controls[i], PROBE_TIMEOUT_MS) == ESP_OK) {
            snprintf(out + n, cap - (size_t)n, " noisy");
            goto done;
        }
    }
    int own = 0, all = 0;
    uint8_t addrs[5];
    memcpy(addrs, b->own, 2);
    memcpy(addrs + 2, b->shared, 3);
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (!addrs[i] || i2c_master_probe(bus, addrs[i], PROBE_TIMEOUT_MS) != ESP_OK) continue;
        all++;
        if (i < 2) own++;
        int m = snprintf(out + n, cap - (size_t)n, " 0x%02X", addrs[i]);
        if (m > 0 && (size_t)(n + m) < cap) n += m;
    }
    if (!all) snprintf(out + n, cap - (size_t)n, " none");
    verdict = own >= 1 && all >= 2;
done:
    i2c_del_master_bus(bus);
release:
    gpio_reset_pin(b->sda);
    gpio_reset_pin(b->scl);
    return verdict;
}

static int board_index(const char *id)
{
    if (!id) return -1;
    for (size_t i = 0; i < BOARD_COUNT; i++) {
        if (!strcmp(k_boards[i].id, id)) return (int)i;
    }
    return -1;
}

static void nvs_sync(bool conclusive)
{
    esp_err_t err = nvs_flash_init();   /* net_wifi_init repeats it and repairs a full partition */
    if (err != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char stored[48] = "";
    size_t len = sizeof(stored);
    if (nvs_get_str(h, NVS_KEY_HW, stored, &len) != ESP_OK) stored[0] = 0;
    len = sizeof(s_rejected);
    if (nvs_get_str(h, NVS_KEY_REJECTED, s_rejected, &len) != ESP_OK) s_rejected[0] = 0;
    if (conclusive) {
        if (strcmp(stored, k_boards[s_hw].id) != 0 &&
            nvs_set_str(h, NVS_KEY_HW, k_boards[s_hw].id) == ESP_OK) {
            (void)nvs_commit(h);
        }
    } else if (board_index(stored) >= 0) {
        s_hw = board_index(stored);
        s_source = "nvs";
    }
    nvs_close(h);
}

void kbx_board_identity_probe(void)
{
    if (s_probed) return;
    s_probed = true;
    size_t used = 0;
    s_detail[0] = 0;
    /* The compiled board's pins first; another board's only if they fail.
     * A probe can see a glitch (a stray ACK trips the controls): the compiled
     * board gets three tries and matches on the first clean one, another
     * board must match twice in a row before it names the hardware. */
    for (size_t step = 0; step < BOARD_COUNT && s_hw < 0; step++) {
        size_t i = step == 0 ? COMPILED_INDEX : (step <= COMPILED_INDEX ? step - 1 : step);
        const int tries = step == 0 ? 3 : 2;
        int hits = 0, tried = 0;
        if (used) {
            int m = snprintf(s_detail + used, sizeof(s_detail) - used, " | ");
            if (m > 0 && used + (size_t)m < sizeof(s_detail)) used += (size_t)m;
        }
        for (tried = 1; tried <= tries; tried++) {
            s_detail[used] = 0;   /* the last attempt's devices stay in the detail */
            hits += probe_pair(&k_boards[i], s_detail + used, sizeof(s_detail) - used);
            if (step == 0 ? hits >= 1 : hits < tried) break;
        }
        used = strlen(s_detail);
        if (tried > 1 && used + 8 < sizeof(s_detail)) {
            used += (size_t)snprintf(s_detail + used, sizeof(s_detail) - used, " (try %d)", tried > tries ? tries : tried);
        }
        if (step == 0 ? hits >= 1 : hits == tries) {
            s_hw = (int)i;
            s_source = "i2c";
        }
    }
    nvs_sync(s_hw >= 0);
    /* Battery power: only a board with a latch keeps it, and only its pin. */
    const board_desc_t *eff = &k_boards[s_hw >= 0 ? s_hw : COMPILED_INDEX];
    if (eff->power_latch != GPIO_NUM_NC) {
        gpio_reset_pin(eff->power_latch);
        gpio_set_direction(eff->power_latch, GPIO_MODE_OUTPUT);
        gpio_set_level(eff->power_latch, 1);
    }
    kbx_board_identity_log();
}

const char *kbx_board_compiled_id(void)
{
    return CONFIG_KBX_BOARD_ID;
}

const char *kbx_board_probed_id(void)
{
    return s_hw >= 0 ? k_boards[s_hw].id : NULL;
}

const char *kbx_board_effective_id(void)
{
    return s_hw >= 0 ? k_boards[s_hw].id : CONFIG_KBX_BOARD_ID;
}

const char *kbx_board_probe_source(void)
{
    return s_source;
}

const char *kbx_board_probe_detail(void)
{
    return s_detail;
}

bool kbx_board_mismatch(void)
{
    return s_hw >= 0 && s_hw != COMPILED_INDEX;
}

const char *kbx_board_usb_serial(void)
{
    return s_hw >= 0 ? k_boards[s_hw].usb_serial : CONFIG_UAC_TUSB_SERIAL_NUM;
}

bool kbx_board_id_known(const char *id)
{
    return board_index(id) >= 0;
}

void kbx_board_identity_log(void)
{
    const char *probed = kbx_board_probed_id();
    if (kbx_board_mismatch()) {
        ESP_LOGE(TAG, "board: compiled=%s probed=%s source=%s mismatch=1 board_mismatch (%s)",
                 kbx_board_compiled_id(), probed, s_source, s_detail);
    } else {
        ESP_LOGI(TAG, "board: compiled=%s probed=%s source=%s mismatch=0 (%s)", kbx_board_compiled_id(),
                 probed ? probed : "unknown", s_source, s_detail);
    }
    if (s_rejected[0]) ESP_LOGW(TAG, "board: last rejected image %s", s_rejected);
}

void kbx_board_note_rejected(const char *version)
{
    snprintf(s_rejected, sizeof(s_rejected), "%.31s for %.40s", version ? version : "?", kbx_board_compiled_id());
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, NVS_KEY_REJECTED, s_rejected) == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
}

const char *kbx_board_rejected(void)
{
    return s_rejected;
}

/* KBX hw?: the one line host onboarding reads. */
void kbx_board_hw_log(void)
{
    uint8_t mac[6] = {0};
    char mac_s[18] = "?";
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(mac_s, sizeof(mac_s), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
    }
    const char *probed = kbx_board_probed_id();
    ESP_LOGI("kbx_hw", "hw: chip=" CONFIG_IDF_TARGET " mac=%s board=%s image=%s match=%s via=%s", mac_s,
             probed ? probed : "unknown", kbx_board_compiled_id(),
             !probed ? "unknown" : (kbx_board_mismatch() ? "no" : "yes"), s_source);
}
