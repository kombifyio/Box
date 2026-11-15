/*
 * kombify box - account device settings (speechkit.box_settings.v1).
 * Contract and conflict rule: device_config.h.
 *
 * Threads: apply runs on the CDC task and on the edge_sync task (serialized
 * by s_apply_lock); local changes come from LVGL events and the CDC task.
 * Flash commits never run in an LVGL event, so the sync state is persisted
 * on the FreeRTOS timer task.
 */
#include "device_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "fw_update_manifest.h"
#include "audio.h"
#include "board_identity.h"
#include "display_design.h"
#include "i18n.h"
#include "net_ws.h"
#include "wakeword.h"

static const char *TAG = "kbx_cfg";

#define NVS_NS          "kbx_cfg"
#define NVS_KEY_REV     "rev_v1"
#define NVS_KEY_PENDING "pending_v1"
#define NVS_KEY_FW      "fw_v1"        /* blob: the account's firmware policy */
#define NVS_KEY_FEED    "feed_v1"      /* blob: the account's Feed popup policy */
#define REVISION_MAX    0x7fffffffUL

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_apply_lock;
static uint32_t s_applied_rev;
static bool s_pending;
static uint32_t s_local_gen;   /* local changes so far */
static uint32_t s_gen;         /* resets so far: an apply that started before one is dropped */
static device_config_firmware_t s_fw;   /* account-owned, guarded by s_lock (a few bytes) */
static device_config_feed_t s_feed;     /* account-owned, guarded by s_lock */
/* Wakeword shortcuts of the account document (wake.shortcuts), guarded by
 * s_lock. Account-owned like the firmware policy: the account binds a wake
 * phrase to an assistant target and an entry mode, the board only carries it. */
static device_config_wake_shortcut_t s_shortcuts[DEVICE_CONFIG_SHORTCUT_MAX];
static uint8_t s_shortcut_count;

static const char *const k_locales[] = { "en", "de", "es", "zh-Hans", "ar", "hi" };
#define LOCALE_COUNT (sizeof(k_locales) / sizeof(k_locales[0]))

static void persist_cb(void *arg1, uint32_t arg2)
{
    (void)arg1;
    (void)arg2;
    taskENTER_CRITICAL(&s_lock);
    uint32_t rev = s_applied_rev;
    uint8_t pending = s_pending ? 1 : 0;
    device_config_firmware_t fw = s_fw;
    device_config_feed_t feed = s_feed;
    taskEXIT_CRITICAL(&s_lock);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "config: persist failed (nvs open)");
        return;
    }
    esp_err_t err = nvs_set_u32(h, NVS_KEY_REV, rev);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_PENDING, pending);
    /* The effective firmware policy is restored with the applied revision:
     * a pin the account set survives a reboot even if later documents omit it. */
    if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_FW, &fw, sizeof(fw));
    if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_FEED, &feed, sizeof(feed));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "config: persist failed (%s)", esp_err_to_name(err));
}

static void persist_async(void)
{
    /* Latest state wins: the callback snapshots under the lock. */
    if (xTimerPendFunctionCall(persist_cb, NULL, 0, 0) != pdPASS) {
        ESP_LOGW(TAG, "config: persist queue full, kept in RAM until the next change");
    }
}

esp_err_t device_config_init(void)
{
    if (!s_apply_lock) s_apply_lock = xSemaphoreCreateMutex();
    if (!s_apply_lock) return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    uint32_t rev = 0;
    uint8_t pending = 0;
    device_config_firmware_t fw;
    memset(&fw, 0, sizeof(fw));
    device_config_feed_t feed;
    memset(&feed, 0, sizeof(feed));
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, NVS_KEY_REV, &rev);
        (void)nvs_get_u8(h, NVS_KEY_PENDING, &pending);
        size_t fw_len = sizeof(fw);
        device_config_firmware_t stored;
        if (nvs_get_blob(h, NVS_KEY_FW, &stored, &fw_len) == ESP_OK && fw_len == sizeof(stored) &&
            memchr(stored.target_version, 0, sizeof(stored.target_version)) &&
            (!stored.has_target || fw_update_version_valid(stored.target_version))) {
            fw = stored;
        }
        size_t feed_len = sizeof(feed);
        device_config_feed_t stored_feed;
        if (nvs_get_blob(h, NVS_KEY_FEED, &stored_feed, &feed_len) == ESP_OK && feed_len == sizeof(stored_feed) &&
            stored_feed.quiet_from_min < 1440 && stored_feed.quiet_to_min < 1440) {
            feed = stored_feed;
        }
        nvs_close(h);
    }
    if (rev > REVISION_MAX) rev = 0;
    if (rev == 0) {
        memset(&fw, 0, sizeof(fw));
        memset(&feed, 0, sizeof(feed));
    }
    taskENTER_CRITICAL(&s_lock);
    s_applied_rev = rev;
    s_pending = pending != 0;
    s_fw = fw;
    s_feed = feed;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "config: applied revision %lu%s", (unsigned long)rev,
             pending ? ", local change pending" : "");
    return ESP_OK;
}

static bool phrase_offered(const char *id)
{
    int n = wakeword_phrase_count();
    for (int i = 0; i < n; i++) {
        const char *offered = wakeword_phrase_id(i);
        if (offered && strcmp(offered, id) == 0) return true;
    }
    return false;
}

/* Validated desired values; has_* false leaves the device value as is. */
typedef struct {
    uint32_t revision;
    bool has_enabled;
    bool enabled;
    bool has_phrase;
    char phrase[16];
    /* wake.shortcuts: the whole member, replaced when a document carries it. */
    bool has_shortcuts;
    device_config_wake_shortcut_t shortcuts[DEVICE_CONFIG_SHORTCUT_MAX];
    uint8_t shortcut_count;
    bool has_followups;
    unsigned max_followups;
    bool has_conversation;
    bool conversation;
    bool has_turn_max;
    unsigned turn_max_s;
    bool has_cues;
    bool cues;
    bool has_cue_style;
    char cue_style[12];
    bool has_voice;
    char voice[8];
    bool has_volume;
    unsigned volume;
    bool has_locale;
    char locale[12];
    bool has_design;
    kbx_design_t design;
    /* firmware member: account-owned, never conflicts with a local change */
    bool has_fw_target;     /* target_version present (string or null) */
    bool fw_target_set;     /* ... and a string */
    char fw_target[32];
    bool has_fw_auto;
    bool fw_auto;
    bool has_fw_request;
    uint32_t fw_request;
    /* feed member: account-owned like firmware */
    bool has_feed;
    bool feed_popup;        /* absent popup = on */
    bool feed_quiet;
    uint16_t feed_from;
    uint16_t feed_to;
} desired_t;

/* "HH:MM" -> minute of day, or -1. */
static int clock_minutes(const cJSON *v)
{
    if (!cJSON_IsString(v)) return -1;
    const char *s = v->valuestring;
    if (strlen(s) != 5 || s[2] != ':') return -1;
    for (int i = 0; i < 5; i++) {
        if (i != 2 && (s[i] < '0' || s[i] > '9')) return -1;
    }
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    return h < 24 && m < 60 ? h * 60 + m : -1;
}

static bool is_whole(double v, double max)
{
    return v >= 0.0 && v <= max && v == (double)(uint32_t)v;
}

/* A registered agent id, the charset the Gateway accepts for a shortcut
 * target (lowercase, digits, dash; at least one character). */
static bool target_id_valid(const char *id)
{
    if (!id || !id[0]) return false;
    size_t n = 0;
    for (const char *p = id; *p; ++p, ++n) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
    }
    return n <= 128;
}

/* The wakeword shortcuts of one document. Closed: an unknown member, a phrase
 * this firmware does not offer, a bad target, a bad mode or a second binding
 * for one phrase refuses the whole document, so a typo never silently leaves
 * the board on the default target. */
static const char *parse_shortcuts(const cJSON *list, desired_t *d)
{
    if (!cJSON_IsArray(list)) return "wake.shortcuts";
    int count = cJSON_GetArraySize(list);
    if (count < 0 || count > DEVICE_CONFIG_SHORTCUT_MAX) return "wake.shortcuts";
    for (int i = 0; i < count; i++) {
        const cJSON *entry = cJSON_GetArrayItem(list, i);
        if (!cJSON_IsObject(entry)) return "wake.shortcuts";
        const cJSON *phrase = cJSON_GetObjectItemCaseSensitive(entry, "phrase");
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(entry, "target");
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(entry, "mode");
        const cJSON *unknown = NULL;
        const cJSON *member = NULL;
        cJSON_ArrayForEach(member, entry) {
            const char *key = member->string;
            if (!key || (strcmp(key, "phrase") && strcmp(key, "target") && strcmp(key, "mode"))) {
                unknown = member;
                break;
            }
        }
        if (unknown) return "wake.shortcuts.unknown";
        if (!cJSON_IsString(phrase) || !phrase->valuestring[0] ||
            strlen(phrase->valuestring) >= sizeof(d->shortcuts[0].phrase)) {
            return "wake.shortcuts.phrase";
        }
        if (!phrase_offered(phrase->valuestring)) {
            /* Closed set: only phrases this firmware runs. */
            return "wake.shortcuts.phrase";
        }
        for (int j = 0; j < d->shortcut_count; j++) {
            if (strcmp(d->shortcuts[j].phrase, phrase->valuestring) == 0) {
                /* One phrase, one meaning. */
                return "wake.shortcuts.phrase";
            }
        }
        device_config_wake_shortcut_t shortcut;
        memset(&shortcut, 0, sizeof(shortcut));
        strlcpy(shortcut.phrase, phrase->valuestring, sizeof(shortcut.phrase));
        if (target && !cJSON_IsNull(target)) {
            if (!cJSON_IsString(target) || !target_id_valid(target->valuestring)) {
                return "wake.shortcuts.target";
            }
            strlcpy(shortcut.target, target->valuestring, sizeof(shortcut.target));
        }
        if (mode && !cJSON_IsNull(mode)) {
            if (!cJSON_IsString(mode)) return "wake.shortcuts.mode";
            if (strcmp(mode->valuestring, "oneshot") == 0) {
                shortcut.conversation = false;
            } else if (strcmp(mode->valuestring, "conversation") == 0) {
                shortcut.conversation = true;
            } else {
                return "wake.shortcuts.mode";
            }
        }
        d->shortcuts[d->shortcut_count++] = shortcut;
    }
    return NULL;
}

static const char *parse_desired(const char *json, size_t len, desired_t *d)
{
    memset(d, 0, sizeof(*d));
    if (!json || len == 0) return "empty";
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return "json";
    const char *reason = NULL;
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(root, "revision");
    const cJSON *wake = cJSON_GetObjectItemCaseSensitive(root, "wake");
    const cJSON *assist = cJSON_GetObjectItemCaseSensitive(root, "assist");
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    const cJSON *locale = cJSON_GetObjectItemCaseSensitive(root, "locale");
    const cJSON *firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    const cJSON *feed = cJSON_GetObjectItemCaseSensitive(root, "feed");
    if (!cJSON_IsObject(root)) {
        reason = "not_object";
    } else if (!cJSON_IsString(schema) || strcmp(schema->valuestring, DEVICE_CONFIG_SCHEMA) != 0) {
        reason = "schema";
    } else if (!cJSON_IsNumber(rev) || rev->valuedouble < 1.0 ||
               rev->valuedouble > (double)REVISION_MAX ||
               rev->valuedouble != (double)(uint32_t)rev->valuedouble) {
        reason = "revision";
    } else if (wake && !cJSON_IsObject(wake)) {
        reason = "wake";
    } else if (assist && !cJSON_IsObject(assist)) {
        reason = "assist";
    } else if (audio && !cJSON_IsObject(audio)) {
        reason = "audio";
    } else if (firmware && !cJSON_IsObject(firmware)) {
        reason = "firmware";
    } else if (display && !cJSON_IsObject(display)) {
        reason = "display";
    } else if (feed && !cJSON_IsObject(feed)) {
        reason = "feed";
    } else {
        d->revision = (uint32_t)rev->valuedouble;
        const cJSON *enabled = wake ? cJSON_GetObjectItemCaseSensitive(wake, "enabled") : NULL;
        const cJSON *phrase = wake ? cJSON_GetObjectItemCaseSensitive(wake, "phrase") : NULL;
        const cJSON *shortcuts = wake ? cJSON_GetObjectItemCaseSensitive(wake, "shortcuts") : NULL;
        const cJSON *mf = assist ? cJSON_GetObjectItemCaseSensitive(assist, "max_followups") : NULL;
        const cJSON *cv = assist ? cJSON_GetObjectItemCaseSensitive(assist, "conversation") : NULL;
        const cJSON *tm = assist ? cJSON_GetObjectItemCaseSensitive(assist, "turn_max_s") : NULL;
        const cJSON *sc = assist ? cJSON_GetObjectItemCaseSensitive(assist, "sound_cues") : NULL;
        const cJSON *ss = assist ? cJSON_GetObjectItemCaseSensitive(assist, "sound_cue_style") : NULL;
        const cJSON *vo = assist ? cJSON_GetObjectItemCaseSensitive(assist, "voice") : NULL;
        const cJSON *vol = audio ? cJSON_GetObjectItemCaseSensitive(audio, "volume") : NULL;
        const cJSON *fwch = firmware ? cJSON_GetObjectItemCaseSensitive(firmware, "channel") : NULL;
        const cJSON *fwtg = firmware ? cJSON_GetObjectItemCaseSensitive(firmware, "target_version") : NULL;
        const cJSON *fwau = firmware ? cJSON_GetObjectItemCaseSensitive(firmware, "auto_update") : NULL;
        const cJSON *fwrq = firmware ? cJSON_GetObjectItemCaseSensitive(firmware, "update_request") : NULL;
        const cJSON *dsg = display ? cJSON_GetObjectItemCaseSensitive(display, "design") : NULL;
        const cJSON *fdp = feed ? cJSON_GetObjectItemCaseSensitive(feed, "popup") : NULL;
        const cJSON *fdf = feed ? cJSON_GetObjectItemCaseSensitive(feed, "quiet_from") : NULL;
        const cJSON *fdt = feed ? cJSON_GetObjectItemCaseSensitive(feed, "quiet_to") : NULL;
        const int feed_from = clock_minutes(fdf), feed_to = clock_minutes(fdt);
        kbx_design_t design_value = KBX_DESIGN_CLASSIC;
        const bool design_ok = dsg && cJSON_IsString(dsg) && kbx_design_from_id(dsg->valuestring, &design_value);
        bool locale_ok = false;
        for (size_t i = 0; locale && cJSON_IsString(locale) && i < LOCALE_COUNT; i++) {
            if (!strcmp(locale->valuestring, k_locales[i])) locale_ok = true;
        }
        bool voice_ok = false;
        for (int i = 0; vo && cJSON_IsString(vo) && i < NET_WS_VOICE_COUNT; i++) {
            if (!strcmp(vo->valuestring, net_ws_voice_id((net_ws_voice_t)i))) voice_ok = true;
        }
        bool style_ok = false;
        for (int i = 0; ss && cJSON_IsString(ss) && i < AUDIO_CUE_STYLE_COUNT; i++) {
            if (!strcmp(ss->valuestring, audio_cue_style_id((audio_cue_style_t)i))) style_ok = true;
        }
        if (enabled && !cJSON_IsBool(enabled)) {
            reason = "wake.enabled";
        } else if (mf && (!cJSON_IsNumber(mf) || mf->valuedouble < 0.0 || mf->valuedouble > 3.0 ||
                          mf->valuedouble != (double)(int)mf->valuedouble)) {
            reason = "assist.max_followups";
        } else if (cv && !cJSON_IsBool(cv)) {
            reason = "assist.conversation";
        } else if (tm && (!cJSON_IsNumber(tm) || tm->valuedouble < 30.0 || tm->valuedouble > 3600.0 ||
                          tm->valuedouble != (double)(int)tm->valuedouble)) {
            reason = "assist.turn_max_s";
        } else if (sc && !cJSON_IsBool(sc)) {
            reason = "assist.sound_cues";
        } else if (ss && !style_ok) {
            /* Closed set: chime, pop, glass, bell. */
            reason = "assist.sound_cue_style";
        } else if (vo && !voice_ok) {
            /* Closed set: auto, female, male. */
            reason = "assist.voice";
        } else if (vol && (!cJSON_IsNumber(vol) || !is_whole(vol->valuedouble, 100.0))) {
            reason = "audio.volume";
        } else if (locale && !locale_ok) {
            reason = "locale";
        } else if (dsg && !design_ok) {
            /* Closed set: classic, watch. */
            reason = "display.design";
        } else if (fwch && (!cJSON_IsString(fwch) || strcmp(fwch->valuestring, "stable"))) {
            /* stable is the only channel that publishes Box firmware. */
            reason = "firmware.channel";
        } else if (fwtg && !cJSON_IsNull(fwtg) &&
                   (!cJSON_IsString(fwtg) || !fw_update_version_valid(fwtg->valuestring))) {
            reason = "firmware.target_version";
        } else if (fwau && !cJSON_IsBool(fwau)) {
            reason = "firmware.auto_update";
        } else if (fwrq && (!cJSON_IsNumber(fwrq) || !is_whole(fwrq->valuedouble, (double)REVISION_MAX))) {
            reason = "firmware.update_request";
        } else if (fdp && !cJSON_IsBool(fdp)) {
            reason = "feed.popup";
        } else if (fdf && feed_from < 0) {
            reason = "feed.quiet_from";
        } else if ((fdt && feed_to < 0) || (!fdf != !fdt)) {
            /* The quiet times come as a pair. */
            reason = "feed.quiet_to";
        } else if (phrase && (!cJSON_IsString(phrase) ||
                              strlen(phrase->valuestring) >= sizeof(d->phrase) ||
                              !phrase_offered(phrase->valuestring))) {
            /* Closed set: only phrases this firmware runs. */
            reason = "wake.phrase";
        } else if (shortcuts) {
            /* The shortcuts are validated (and read) in the same step: a bad
             * one refuses the document. */
            const char *why_shortcut = parse_shortcuts(shortcuts, d);
            if (why_shortcut) reason = why_shortcut;
        }
        if (!reason) {
            if (enabled) {
                d->has_enabled = true;
                d->enabled = cJSON_IsTrue(enabled);
            }
            if (phrase) {
                d->has_phrase = true;
                strlcpy(d->phrase, phrase->valuestring, sizeof(d->phrase));
            }
            if (shortcuts) {
                d->has_shortcuts = true;
            }
            if (vo) {
                d->has_voice = true;
                strlcpy(d->voice, vo->valuestring, sizeof(d->voice));
            }
            if (mf) {
                d->has_followups = true;
                d->max_followups = (unsigned)mf->valuedouble;
            }
            if (cv) {
                d->has_conversation = true;
                d->conversation = cJSON_IsTrue(cv);
            }
            if (tm) {
                d->has_turn_max = true;
                d->turn_max_s = (unsigned)tm->valuedouble;
            }
            if (sc) {
                d->has_cues = true;
                d->cues = cJSON_IsTrue(sc);
            }
            if (ss) {
                d->has_cue_style = true;
                strlcpy(d->cue_style, ss->valuestring, sizeof(d->cue_style));
            }
            if (vol) {
                d->has_volume = true;
                d->volume = (unsigned)vol->valuedouble;
            }
            if (locale) {
                d->has_locale = true;
                strlcpy(d->locale, locale->valuestring, sizeof(d->locale));
            }
            if (dsg) {
                d->has_design = true;
                d->design = design_value;
            }
            if (fwtg) {
                d->has_fw_target = true;
                d->fw_target_set = cJSON_IsString(fwtg);
                if (d->fw_target_set) strlcpy(d->fw_target, fwtg->valuestring, sizeof(d->fw_target));
            }
            if (fwau) {
                d->has_fw_auto = true;
                d->fw_auto = cJSON_IsTrue(fwau);
            }
            if (fwrq) {
                d->has_fw_request = true;
                d->fw_request = (uint32_t)fwrq->valuedouble;
            }
            if (feed) {
                d->has_feed = true;
                d->feed_popup = !fdp || cJSON_IsTrue(fdp);
                d->feed_quiet = fdf && fdt;
                d->feed_from = d->feed_quiet ? (uint16_t)feed_from : 0;
                d->feed_to = d->feed_quiet ? (uint16_t)feed_to : 0;
            }
        }
    }
    cJSON_Delete(root);
    return reason;
}

device_config_outcome_t device_config_apply_json(const char *json, size_t len,
                                                 const char **reason)
{
    return device_config_apply_json_rev(json, len, reason, NULL);
}

static device_config_outcome_t apply_json_with(desired_t *dp, const char *json, size_t len,
                                              const char **reason, uint32_t *revision)
{
    if (reason) *reason = NULL;
    if (revision) *revision = 0;
    if (!s_apply_lock) {
        if (reason) *reason = "not_ready";
        return DEVICE_CONFIG_REJECTED;
    }
    const char *why = parse_desired(json, len, dp);
    if (revision) *revision = dp->revision;
    if (why) {
        ESP_LOGW(TAG, "config: rejected (%s), nothing applied", why);
        if (reason) *reason = why;
        return DEVICE_CONFIG_REJECTED;
    }
    xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    uint32_t applied = s_applied_rev;
    uint32_t gen = s_gen;
    taskEXIT_CRITICAL(&s_lock);
    device_config_outcome_t outcome;
    if (dp->revision < applied) {
        outcome = DEVICE_CONFIG_STALE;
    } else if (dp->revision == applied) {
        outcome = DEVICE_CONFIG_CURRENT;
    } else {
        /* Phrase first, so enabling builds the selected model. */
        if (dp->has_phrase) {
            const char *current = wakeword_phrase_id(wakeword_get_phrase());
            if (!current || strcmp(current, dp->phrase) != 0) {
                (void)wakeword_set_phrase_id(dp->phrase);
            }
        }
        if (dp->has_followups && net_ws_max_followups() != dp->max_followups) {
            (void)net_ws_set_max_followups(dp->max_followups);
        }
        if (dp->has_conversation && net_ws_conversation_default() != dp->conversation) {
            (void)net_ws_set_conversation_default(dp->conversation);
        }
        if (dp->has_turn_max && net_ws_turn_max_seconds() != dp->turn_max_s) {
            (void)net_ws_set_turn_max_seconds(dp->turn_max_s);
        }
        if (dp->has_cues && audio_cues_enabled() != dp->cues) audio_cues_set_enabled(dp->cues);
        if (dp->has_cue_style && strcmp(audio_cue_style_id(audio_cue_style()), dp->cue_style) != 0) {
            (void)audio_cue_set_style_id(dp->cue_style);
        }
        if (dp->has_voice && strcmp(net_ws_voice_id(net_ws_voice()), dp->voice) != 0) {
            (void)net_ws_set_voice_id(dp->voice);
        }
        if (dp->has_volume && audio_volume() != dp->volume) (void)audio_set_volume(dp->volume);
        if (dp->has_locale && strcmp(i18n_locale_tag(i18n_requested_locale()), dp->locale) != 0) {
            (void)i18n_set_locale_tag(dp->locale);
        }
        if (dp->has_enabled) {
            wakeword_status_t st;
            wakeword_get_status(&st);
            if (st.enabled != dp->enabled) (void)wakeword_set_enabled(dp->enabled);
        }
        /* The screen is built once at start: a changed design is stored here and
         * the UI timer restarts the box into it as soon as it is idle. */
        if (dp->has_design && (kbx_design_stored() != dp->design || !kbx_design_chosen())) (void)kbx_design_set(dp->design);
        outcome = DEVICE_CONFIG_APPLIED;
    }
    if (outcome == DEVICE_CONFIG_APPLIED || outcome == DEVICE_CONFIG_CURRENT) {
        /* One commit: the applied revision, the dropped local change and the
         * account-owned firmware policy (taken from every accepted document at
         * or above the applied revision, member by member). A reset since this
         * apply began (another account) discards the whole document. */
        bool dropped = false;
        taskENTER_CRITICAL(&s_lock);
        if (s_gen != gen) {
            outcome = DEVICE_CONFIG_STALE;
            taskEXIT_CRITICAL(&s_lock);
            goto committed;
        }
        if (outcome == DEVICE_CONFIG_APPLIED) {
            dropped = s_pending;
            s_applied_rev = dp->revision;
            s_pending = false;
        }
        if (dp->has_fw_target) {
            s_fw.has_target = dp->fw_target_set;
            strlcpy(s_fw.target_version, dp->fw_target_set ? dp->fw_target : "", sizeof(s_fw.target_version));
        }
        if (dp->has_fw_auto) s_fw.auto_update = dp->fw_auto;
        if (dp->has_fw_request) s_fw.update_request = dp->fw_request;
        if (dp->has_fw_target || dp->has_fw_auto || dp->has_fw_request) s_fw.valid = true;
        s_fw.revision = dp->revision;
        if (dp->has_shortcuts) {
            /* The shortcut member is whole: what it omits is the default. */
            s_shortcut_count = dp->shortcut_count;
            for (uint8_t i = 0; i < dp->shortcut_count; i++) s_shortcuts[i] = dp->shortcuts[i];
        } else {
            s_shortcut_count = 0;
        }
        if (dp->has_feed) {
            /* The feed member is whole: what it omits is the default. */
            s_feed.valid = true;
            s_feed.popup = dp->feed_popup;
            s_feed.quiet = dp->feed_quiet;
            s_feed.quiet_from_min = dp->feed_from;
            s_feed.quiet_to_min = dp->feed_to;
        } else {
            /* An account document without the member means the defaults the
             * Gateway applies too: popups on, no quiet hours, nothing to echo. */
            s_feed.valid = false;
            s_feed.popup = true;
            s_feed.quiet = false;
            s_feed.quiet_from_min = 0;
            s_feed.quiet_to_min = 0;
        }
        taskEXIT_CRITICAL(&s_lock);
        persist_async();
        if (dropped) ESP_LOGI(TAG, "config: account revision is newer, local change replaced");
    }
committed:
    xSemaphoreGive(s_apply_lock);
    ESP_LOGI(TAG, "config: revision %lu %s (was %lu)%s%s%s", (unsigned long)dp->revision,
             device_config_outcome_name(outcome), (unsigned long)applied,
             dp->has_enabled ? (dp->enabled ? " wake=on" : " wake=off") : "",
             dp->has_phrase ? " phrase=" : "", dp->has_phrase ? dp->phrase : "");
    if (dp->has_followups) ESP_LOGI(TAG, "config: assist.max_followups=%u", dp->max_followups);
    if (dp->has_conversation)
        ESP_LOGI(TAG, "config: assist.conversation=%s", dp->conversation ? "on" : "off");
    if (dp->has_turn_max) ESP_LOGI(TAG, "config: assist.turn_max_s=%u", dp->turn_max_s);
    if (dp->has_cues || dp->has_cue_style) {
        ESP_LOGI(TAG, "config: assist.sound_cues=%s style=%s", dp->has_cues ? (dp->cues ? "on" : "off") : "-",
                 dp->has_cue_style ? dp->cue_style : "-");
    }
    if (dp->has_design) ESP_LOGI(TAG, "config: display.design=%s", kbx_design_id(dp->design));
    if (dp->has_volume || dp->has_locale) {
        ESP_LOGI(TAG, "config: volume=%d locale=%s",
                 dp->has_volume ? (int)dp->volume : -1, dp->has_locale ? dp->locale : "-");
    }
    if (dp->has_feed) {
        ESP_LOGI(TAG, "config: feed popup=%s quiet=%s %02u:%02u-%02u:%02u", dp->feed_popup ? "on" : "off",
                 dp->feed_quiet ? "on" : "off", dp->feed_from / 60u, dp->feed_from % 60u, dp->feed_to / 60u, dp->feed_to % 60u);
    }
    if (dp->has_fw_target || dp->has_fw_auto || dp->has_fw_request) {
        ESP_LOGI(TAG, "config: firmware target=%s auto_update=%d update_request=%ld",
                 dp->has_fw_target ? (dp->fw_target_set ? dp->fw_target : "latest") : "-",
                 dp->has_fw_auto ? (int)dp->fw_auto : -1, dp->has_fw_request ? (long)dp->fw_request : -1L);
    }
    return outcome;
}

/* The parsed document (up to eight shortcuts) is about 1.5 KB: it lives on the
 * heap, because KBX config apply runs on the 4 KB TinyUSB task stack. */
device_config_outcome_t device_config_apply_json_rev(const char *json, size_t len,
                                                     const char **reason, uint32_t *revision)
{
    desired_t *dp = heap_caps_calloc(1, sizeof(*dp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dp) {
        if (reason) *reason = "no_memory";
        if (revision) *revision = 0;
        return DEVICE_CONFIG_REJECTED;
    }
    device_config_outcome_t outcome = apply_json_with(dp, json, len, reason, revision);
    free(dp);
    return outcome;
}

void device_config_note_local_change(void)
{
    bool changed;
    taskENTER_CRITICAL(&s_lock);
    changed = !s_pending;
    s_pending = true;
    s_local_gen++;
    taskEXIT_CRITICAL(&s_lock);
    if (changed) {
        ESP_LOGI(TAG, "config: local change pending for the account");
        persist_async();
    }
}

void device_config_mark_written(uint32_t revision, const device_config_state_t *snapshot)
{
    if (revision == 0 || revision > REVISION_MAX || !snapshot) return;
    bool kept;
    taskENTER_CRITICAL(&s_lock);
    if (s_gen != snapshot->generation) {
        taskEXIT_CRITICAL(&s_lock);
        return;   /* the account changed while the request ran */
    }
    if (revision > s_applied_rev) s_applied_rev = revision;
    kept = s_local_gen != snapshot->local_gen;   /* a local edit after the snapshot stays pending */
    if (!kept) s_pending = false;
    taskEXIT_CRITICAL(&s_lock);
    persist_async();
    ESP_LOGI(TAG, "config: local values written, account at revision %lu%s", (unsigned long)revision,
             kept ? " (a newer local change stays pending)" : "");
}

void device_config_mark_seeded(uint32_t revision, const device_config_state_t *snapshot)
{
    if (revision == 0 || revision > REVISION_MAX || !snapshot) return;
    bool kept;
    taskENTER_CRITICAL(&s_lock);
    if (s_gen != snapshot->generation) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    s_applied_rev = revision;   /* the account (re)started its numbering from this box */
    kept = s_local_gen != snapshot->local_gen;
    if (!kept) s_pending = false;
    taskEXIT_CRITICAL(&s_lock);
    persist_async();
    ESP_LOGI(TAG, "config: seeded the account at revision %lu%s", (unsigned long)revision,
             kept ? " (a newer local change stays pending)" : "");
}

bool device_config_rebase(uint32_t revision)
{
    if (revision == 0 || revision > REVISION_MAX) return false;
    taskENTER_CRITICAL(&s_lock);
    bool pending = s_pending;
    uint32_t was = s_applied_rev;
    bool lower = revision < was;
    if (lower) s_applied_rev = pending ? revision : revision - 1;
    taskEXIT_CRITICAL(&s_lock);
    if (!lower) return pending;
    persist_async();
    ESP_LOGI(TAG, "config: account at revision %lu below applied %lu: %s", (unsigned long)revision,
             (unsigned long)was, pending ? "the local change is written on it" : "its document applies");
    return pending;
}

void device_config_firmware(device_config_firmware_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_fw;
    taskEXIT_CRITICAL(&s_lock);
}

void device_config_feed(device_config_feed_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_feed;
    taskEXIT_CRITICAL(&s_lock);
    if (!out->valid) out->popup = true;   /* no document named it: popups on */
}

bool device_config_feed_quiet_now(const device_config_feed_t *feed, int minute_of_day)
{
    if (!feed || !feed->quiet || feed->quiet_from_min == feed->quiet_to_min) return false;
    if (feed->quiet_from_min < feed->quiet_to_min) {
        return minute_of_day >= feed->quiet_from_min && minute_of_day < feed->quiet_to_min;
    }
    return minute_of_day >= feed->quiet_from_min || minute_of_day < feed->quiet_to_min;
}

void device_config_reset(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_applied_rev = 0;
    s_pending = false;
    s_gen++;
    memset(&s_fw, 0, sizeof(s_fw));   /* another account's firmware policy is not ours */
    memset(&s_feed, 0, sizeof(s_feed));
    s_shortcut_count = 0;             /* another account's shortcuts are not ours */
    taskEXIT_CRITICAL(&s_lock);
    persist_async();
    ESP_LOGI(TAG, "config: sync state reset (account changed)");
}

/* The wakeword shortcut of a phrase, if the account bound one to it. False
 * when the phrase has no binding: the caller then uses the default target and
 * the default entry mode. */
bool device_config_wake_shortcut(const char *phrase, device_config_wake_shortcut_t *out)
{
    if (!phrase || !out) return false;
    bool found = false;
    taskENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0; i < s_shortcut_count; i++) {
        if (strcmp(s_shortcuts[i].phrase, phrase) == 0) {
            *out = s_shortcuts[i];
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return found;
}

void device_config_get_state(device_config_state_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    out->applied_revision = s_applied_rev;
    out->local_pending = s_pending;
    out->local_gen = s_local_gen;
    out->generation = s_gen;
    taskEXIT_CRITICAL(&s_lock);
}

size_t device_config_local_json(char *out, size_t capacity)
{
    if (!out || capacity == 0) return 0;
    wakeword_status_t st;
    wakeword_get_status(&st);
    const char *phrase = wakeword_phrase_id(wakeword_get_phrase());
    /* The wakeword shortcuts are account-owned like the firmware policy: the
     * board echoes what the account holds so a write-back never clears them. */
    /* The copy and its JSON are 2 KB: on the heap, since KBX sync body and the
     * console run this on the 4 KB TinyUSB task stack. A box that cannot hold
     * them sends no body rather than one that drops the account's shortcuts. */
    struct { device_config_wake_shortcut_t sc[DEVICE_CONFIG_SHORTCUT_MAX]; char json[1024]; } *w =
        heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w) { out[0] = '\0'; return 0; }
    device_config_wake_shortcut_t *shortcuts = w->sc;
    char *shortcuts_json = w->json;
    const size_t shortcuts_cap = sizeof(w->json);
    uint8_t shortcut_count = 0;
    taskENTER_CRITICAL(&s_lock);
    shortcut_count = s_shortcut_count;
    for (uint8_t i = 0; i < shortcut_count; i++) shortcuts[i] = s_shortcuts[i];
    taskEXIT_CRITICAL(&s_lock);
    size_t used = 0;
    if (shortcut_count) {
        int m = snprintf(shortcuts_json, shortcuts_cap, ",\"shortcuts\":[");
        if (m < 0 || (size_t)m >= shortcuts_cap) m = -1;
        if (m > 0) {
            used = (size_t)m;
            for (uint8_t i = 0; i < shortcut_count; i++) {
                m = snprintf(shortcuts_json + used, shortcuts_cap - used,
                             "%s{\"phrase\":\"%s\",\"target\":\"%s\",\"mode\":\"%s\"}",
                             i ? "," : "", shortcuts[i].phrase, shortcuts[i].target,
                             shortcuts[i].conversation ? "conversation" : "oneshot");
                if (m < 0 || (size_t)m >= shortcuts_cap - used) break;
                used += (size_t)m;
            }
            if (m > 0 && (size_t)m < shortcuts_cap - used) {
                /* used already counts every entry; adding the last length again
                 * left NULs before the closing bracket and cut the string short */
                m = snprintf(shortcuts_json + used, shortcuts_cap - used, "]");
                if (m > 0 && (size_t)m < shortcuts_cap - used) {
                    used += (size_t)m;
                    shortcuts_json[used] = '\0';
                } else {
                    shortcuts_json[0] = '\0';
                }
            } else {
                shortcuts_json[0] = '\0';
            }
        } else {
            shortcuts_json[0] = '\0';
        }
    }
    int n = snprintf(out, capacity,
                     "{\"schema\":\"" DEVICE_CONFIG_SCHEMA "\",\"wake\":{\"enabled\":%s,\"phrase\":\"%s\"%s},"
                     "\"assist\":{\"max_followups\":%u,\"sound_cues\":%s,\"sound_cue_style\":\"%s\",\"voice\":\"%s\"},"
                     "\"audio\":{\"volume\":%u},\"locale\":\"%s\"%s%s%s}",
                     st.enabled ? "true" : "false", phrase ? phrase : "", shortcuts_json,
                     net_ws_max_followups(),
                     audio_cues_enabled() ? "true" : "false", audio_cue_style_id(audio_cue_style()),
                     net_ws_voice_id(net_ws_voice()),
                     (unsigned)audio_volume(), i18n_locale_tag(i18n_requested_locale()),
                     kbx_design_chosen() ? ",\"display\":{\"design\":\"" : "",
                     kbx_design_chosen() ? kbx_design_id(kbx_design_stored()) : "",
                     kbx_design_chosen() ? "\"}" : "");
    free(w);
    if (n < 0 || (size_t)n >= capacity) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

size_t device_config_writeback_json(char *out, size_t capacity)
{
    size_t n = device_config_local_json(out, capacity);
    if (n == 0 || out[n - 1] != '}') return 0;
    device_config_firmware_t fw;
    device_config_firmware(&fw);
    /* The account owns the firmware policy: echo what it holds so a
     * write-back never resets it (seed: stable, no pin, no auto update). */
    char target[48];
    if (fw.has_target) snprintf(target, sizeof(target), "\"%s\"", fw.target_version);
    else snprintf(target, sizeof(target), "null");
    /* Same for the Feed popup policy, once the account named one. */
    device_config_feed_t feed;
    device_config_feed(&feed);
    char feed_json[128] = "";
    if (feed.valid && feed.quiet) {
        snprintf(feed_json, sizeof(feed_json),
                 ",\"feed\":{\"popup\":%s,\"quiet_from\":\"%02u:%02u\",\"quiet_to\":\"%02u:%02u\"}",
                 feed.popup ? "true" : "false", feed.quiet_from_min / 60u, feed.quiet_from_min % 60u,
                 feed.quiet_to_min / 60u, feed.quiet_to_min % 60u);
    } else if (feed.valid) {
        snprintf(feed_json, sizeof(feed_json), ",\"feed\":{\"popup\":%s}", feed.popup ? "true" : "false");
    }
    int m = snprintf(out + n - 1, capacity - n + 1,
                     ",\"firmware\":{\"channel\":\"stable\",\"target_version\":%s,\"auto_update\":%s,\"update_request\":%lu}%s}",
                     target, fw.auto_update ? "true" : "false", (unsigned long)fw.update_request, feed_json);
    if (m < 0 || (size_t)m >= capacity - n + 1) {
        out[0] = '\0';
        return 0;
    }
    return n - 1 + (size_t)m;
}

/* "board_identity":{...}, the image's and the hardware's board. */
static int board_identity_json(char *out, size_t capacity)
{
    const char *probed = kbx_board_probed_id();
    const char *rejected = kbx_board_rejected();
    return snprintf(out, capacity,
                    "\"board_identity\":{\"compiled\":\"%s\",\"probed\":\"%s\",\"source\":\"%s\",\"mismatch\":%s%s%s%s}",
                    kbx_board_compiled_id(), probed ? probed : "unknown", kbx_board_probe_source(),
                    kbx_board_mismatch() ? "true" : "false", rejected[0] ? ",\"rejected_image\":\"" : "",
                    rejected, rejected[0] ? "\"" : "");
}

size_t device_config_reported_body(char *out, size_t capacity)
{
    if (!out || capacity == 0) return 0;
    /* The shortcut copy is 1.2 KB: on the heap (the console runs this on the 4 KB TinyUSB stack). */
    device_config_wake_shortcut_t *shortcuts = NULL;
    if (kbx_board_mismatch()) {
        /* Another board's image: no setting applies, the dashboard shows the
         * mismatch and no setting rows. */
        int m = snprintf(out, capacity, "{\"board_mismatch\":true,");
        if (m < 0 || (size_t)m >= capacity) goto truncated;
        int k = board_identity_json(out + m, capacity - (size_t)m);
        if (k < 0 || (size_t)k >= capacity - (size_t)m) goto truncated;
        m += k;
        k = snprintf(out + m, capacity - (size_t)m, ",\"supports\":[]}");
        if (k < 0 || (size_t)k >= capacity - (size_t)m) goto truncated;
        return (size_t)(m + k);
    }
    wakeword_status_t st;
    wakeword_get_status(&st);
    const char *phrase = wakeword_phrase_id(wakeword_get_phrase());
    size_t used = 0;
    int n = snprintf(out, capacity,
                     "{\"wake\":{\"enabled\":%s,\"ready\":%s,\"phrase\":\"%s\",\"error\":\"%s\",\"phrases\":[",
                     st.enabled ? "true" : "false", st.ready ? "true" : "false",
                     phrase ? phrase : "", esp_err_to_name(st.error));
    if (n < 0 || (size_t)n >= capacity) goto truncated;
    used = (size_t)n;
    int count = wakeword_phrase_count();
    for (int i = 0; i < count; i++) {
        const char *id = wakeword_phrase_id(i);
        n = snprintf(out + used, capacity - used, "%s\"%s\"", i ? "," : "", id ? id : "");
        if (n < 0 || (size_t)n >= capacity - used) goto truncated;
        used += (size_t)n;
    }
    n = snprintf(out + used, capacity - used,
                 "],\"shortcuts\":[");
    if (n < 0 || (size_t)n >= capacity - used) goto truncated;
    used += (size_t)n;
    /* The shortcuts the account bound, as the board holds them. The target is
     * an assistant id the board only names: the mint that carries it goes
     * through the ordinary registered-assistant authorization. */
    shortcuts = heap_caps_calloc(DEVICE_CONFIG_SHORTCUT_MAX, sizeof(*shortcuts), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!shortcuts) goto truncated;
    uint8_t shortcut_count = 0;
    taskENTER_CRITICAL(&s_lock);
    shortcut_count = s_shortcut_count;
    for (uint8_t i = 0; i < shortcut_count; i++) shortcuts[i] = s_shortcuts[i];
    taskEXIT_CRITICAL(&s_lock);
    for (uint8_t i = 0; i < shortcut_count; i++) {
        n = snprintf(out + used, capacity - used,
                     "%s{\"phrase\":\"%s\",\"target\":\"%s\",\"mode\":\"%s\"}",
                     i ? "," : "", shortcuts[i].phrase, shortcuts[i].target,
                     shortcuts[i].conversation ? "conversation" : "oneshot");
        if (n < 0 || (size_t)n >= capacity - used) goto truncated;
        used += (size_t)n;
    }
    n = snprintf(out + used, capacity - used,
                 "]},\"assist\":{\"max_followups\":%u,\"conversation\":%s,\"turn_max_s\":%u,"
                 "\"sound_cues\":%s,\"sound_cue_style\":\"%s\",\"sound_cue_styles\":[\"chime\",\"pop\",\"glass\",\"bell\"],"
                 "\"voice\":\"%s\",\"voices\":[\"auto\",\"female\",\"male\"]},"
                 "\"audio\":{\"volume\":%u},\"locale\":\"%s\",\"locales\":[",
                 net_ws_max_followups(), net_ws_conversation_default() ? "true" : "false",
                 net_ws_turn_max_seconds(), audio_cues_enabled() ? "true" : "false",
                 audio_cue_style_id(audio_cue_style()), net_ws_voice_id(net_ws_voice()),
                 (unsigned)audio_volume(),
                 i18n_locale_tag(i18n_requested_locale()));
    if (n < 0 || (size_t)n >= capacity - used) goto truncated;
    used += (size_t)n;
    for (size_t i = 0; i < LOCALE_COUNT; i++) {
        n = snprintf(out + used, capacity - used, "%s\"%s\"", i ? "," : "", k_locales[i]);
        if (n < 0 || (size_t)n >= capacity - used) goto truncated;
        used += (size_t)n;
    }
    /* The settings this box applies: the dashboard shows exactly these rows. */
    n = snprintf(out + used, capacity - used,
                 "],\"display\":{\"design\":\"%s\",\"designs\":[\"classic\",\"watch\"]},"
                 "\"supports\":[\"wake.enabled\",\"wake.phrase\",\"wake.shortcuts\","
                 "\"assist.max_followups\","
                 "\"assist.conversation\",\"assist.turn_max_s\",\"assist.sound_cues\","
                 "\"assist.sound_cue_style\",\"assist.voice\",\"audio.volume\",\"locale\","
                 "\"firmware.channel\",\"firmware.target_version\",\"firmware.auto_update\","
                 "\"firmware.update_request\",\"display.design\",\"feed.popup\",\"feed.quiet_from\",\"feed.quiet_to\"],",
                 kbx_design_id(kbx_design_stored()));
    if (n < 0 || (size_t)n >= capacity - used) goto truncated;
    used += (size_t)n;
    n = board_identity_json(out + used, capacity - used);
    if (n < 0 || (size_t)n + 1 >= capacity - used) goto truncated;
    used += (size_t)n;
    out[used++] = '}';
    out[used] = '\0';
    free(shortcuts);
    return used;
truncated:
    free(shortcuts);
    out[0] = '\0';
    return 0;
}

size_t device_config_reported_json(char *out, size_t capacity)
{
    if (!out || capacity == 0) return 0;
    const esp_app_desc_t *app = esp_app_get_description();
    char elf[17];
    for (int i = 0; i < 8; i++) snprintf(elf + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    char *body = heap_caps_malloc(DEVICE_CONFIG_REPORTED_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        out[0] = '\0';
        return 0;
    }
    size_t body_len = device_config_reported_body(body, DEVICE_CONFIG_REPORTED_CAP);
    int n = body_len ? snprintf(out, capacity,
                                "{\"schema\":\"" DEVICE_CONFIG_SCHEMA "\",\"reported\":{\"board\":\"%s"
                                "\",\"firmware\":\"%s\",\"elf\":\"%s\",%s}", kbx_board_effective_id(), app->version, elf, body + 1)
                     : -1;
    free(body);
    if (n < 0 || (size_t)n >= capacity) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;   /* body + 1 closes "reported", the format closes the root */
}

void device_config_log_status(void)
{
    device_config_state_t state;
    device_config_get_state(&state);
    ESP_LOGI(TAG, "config: schema=" DEVICE_CONFIG_SCHEMA " applied=%lu local_pending=%d",
             (unsigned long)state.applied_revision, (int)state.local_pending);
    char buf[384];
    if (device_config_local_json(buf, sizeof(buf))) ESP_LOGI(TAG, "config: local %s", buf);
    device_config_firmware_t fw;
    device_config_firmware(&fw);
    ESP_LOGI(TAG, "config: firmware valid=%d target=%s auto_update=%d update_request=%lu rev=%lu",
             (int)fw.valid, fw.has_target ? fw.target_version : "latest", (int)fw.auto_update,
             (unsigned long)fw.update_request, (unsigned long)fw.revision);
    /* Reported part as key=value: one CDC log line holds 320 bytes. */
    wakeword_status_t st;
    wakeword_get_status(&st);
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < wakeword_phrase_count() && used + 18 < sizeof(buf); i++) {
        int n = snprintf(buf + used, sizeof(buf) - used, "%s%s", i ? "," : "", wakeword_phrase_id(i));
        if (n < 0) break;
        used += (size_t)n;
    }
    ESP_LOGI(TAG, "config: reported firmware=%s wake enabled=%d ready=%d error=%s phrases=%s",
             esp_app_get_description()->version, (int)st.enabled, (int)st.ready,
             esp_err_to_name(st.error), buf);
}

const char *device_config_outcome_name(device_config_outcome_t outcome)
{
    switch (outcome) {
    case DEVICE_CONFIG_APPLIED: return "applied";
    case DEVICE_CONFIG_CURRENT: return "current";
    case DEVICE_CONFIG_STALE: return "stale";
    case DEVICE_CONFIG_REJECTED: return "rejected";
    }
    return "?";
}
