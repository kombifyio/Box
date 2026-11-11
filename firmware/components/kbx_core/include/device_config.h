/*
 * kombify box - account device settings (speechkit.box_settings.v1).
 *
 * The account keeps the desired assistant settings of a paired box
 * (Companion Studio, Settings > Voice > Box); the box applies them and stays
 * the place where they take effect. Quick Settings and KBX keep working
 * offline: a local change is marked and later written back to the account.
 *
 * Document (desired part, the only part the box applies):
 *   {"schema":"speechkit.box_settings.v1","revision":7,
 *    "wake":{"enabled":true,"phrase":"jarvis"},"assist":{"max_followups":3}}
 *   - schema must match exactly; revision is the account document revision
 *     (Gateway ETag "v<n>"), 1 ... 2^31-1.
 *   - wake.enabled (bool) and wake.phrase (an id this firmware offers, see
 *     KBX wake phrase ?) are optional; an absent field leaves the device
 *     value unchanged.
 *   - assist.max_followups (integer 0-3, optional): follow-up stages after
 *     a reply that expects an answer (KBX assist followups).
 *   - assist.sound_cues (bool, optional, default on) and
 *     assist.sound_cue_style ("chime" | "pop" | "glass" | "bell", optional, default
 *     "chime"): listening earcons (KBX sound cues). The reported part lists
 *     the offered styles in assist.sound_cue_styles.
 *   - audio.volume (0-100) and locale ("en" | "de" | "es" | "zh-Hans" |
 *     "ar" | "hi"), both optional, use the existing setters
 *     (audio_set_volume, i18n_set_locale_tag). An assist.route left in an
 *     older document is ignored: the voice route follows the account
 *     (net_ws.c, D-2026-11-07-09).
 *   - display.design ("classic" | "watch", optional): the screen layout
 *     (display_design.h). The choice is stored and the box restarts into it
 *     once it is idle; the reported part lists display.design and
 *     display.designs, and reported.supports names every setting this box
 *     applies, display.design included.
 *   - firmware {channel:"stable", target_version:null|"x.y.z",
 *     auto_update:bool, update_request:int} is the account's firmware policy,
 *     read through device_config_firmware() by the sync client (edge_sync.c).
 *   - feed {popup:bool, quiet_from:"HH:MM", quiet_to:"HH:MM"} (optional,
 *     popup absent = on; the quiet times come as a pair, box local time) is
 *     the account's Feed popup policy, read through device_config_feed() by
 *     the sync client. Account-owned like firmware: echoed in the write-back.
 *   Any invalid known field rejects the whole document: nothing is applied
 *   partially.
 *
 * Conflict rule (one writer per revision, no clocks):
 *   - revision >  applied: the account changed since this box last synced;
 *     the account wins, the values apply and a pending local change is
 *     dropped (it was made against an older revision).
 *   - revision == applied: nothing to apply; a pending local change is now
 *     uploaded by the sync client with If-Match "v<applied>"; a 412 means
 *     the account moved on and its newer document wins on the next pull.
 *   - revision <  applied: stale (an older replay), ignored.
 *   Forget or a new pairing resets the sync state: a different account's
 *   revisions never compare against the previous one.
 *
 * The device's own fail-safe (an engine that crashes switches the wake word
 * off) is reported, never fought: only a newer revision re-applies it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CONFIG_SCHEMA "speechkit.box_settings.v1"

typedef enum {
    DEVICE_CONFIG_APPLIED = 0,  /* newer revision, values applied */
    DEVICE_CONFIG_CURRENT,      /* same revision, nothing to apply */
    DEVICE_CONFIG_STALE,        /* older revision, ignored */
    DEVICE_CONFIG_REJECTED,     /* invalid document, nothing applied */
} device_config_outcome_t;

/* Firmware policy of the account document (firmware member). Account-owned:
 * it is taken from every accepted document, never conflicts with a local
 * change and never reaches the device settings. */
typedef struct {
    bool valid;                 /* a firmware member was accepted */
    bool has_target;            /* target_version pins a release (else latest) */
    char target_version[32];
    bool auto_update;
    uint32_t update_request;    /* counter: above the handled one = update now */
    uint32_t revision;          /* revision of the last accepted document */
} device_config_firmware_t;

/* Feed popup policy of the account document (feed member). Account-owned
 * like the firmware policy; the defaults (popup on, no quiet hours) hold
 * until a document names it. */
typedef struct {
    bool valid;                 /* a feed member was accepted */
    bool popup;                 /* show new Feed items on the display */
    bool quiet;                 /* quiet hours set */
    uint16_t quiet_from_min;    /* minutes after local midnight */
    uint16_t quiet_to_min;
} device_config_feed_t;

typedef struct {
    uint32_t applied_revision;  /* last account revision applied or written */
    bool local_pending;         /* local change not yet in the account */
    uint32_t local_gen;         /* counts local changes (write-back compares it) */
    uint32_t generation;        /* counts resets: another account, a fresh start */
} device_config_state_t;

esp_err_t device_config_init(void);
/* Applies a desired-settings document (see above). reason is set on reject. */
device_config_outcome_t device_config_apply_json(const char *json, size_t len,
                                                 const char **reason);
/* Same, also returns the document's revision when it could be read (also
 * when a later field rejected it), 0 otherwise. */
device_config_outcome_t device_config_apply_json_rev(const char *json, size_t len,
                                                     const char **reason, uint32_t *revision);
/* Quick Settings or KBX changed a synced setting on the device. */
void device_config_note_local_change(void);
/* The sync client wrote the local values uploaded under snapshot (taken by
 * device_config_get_state before the body was built); the account is now at
 * revision. The pending flag clears only if no local change happened since
 * the snapshot, and nothing changes if the sync state was reset meanwhile. */
void device_config_mark_written(uint32_t revision, const device_config_state_t *snapshot);
/* The account document did not exist (revision 0) and this box created it
 * from its own values: the account is now at revision, whatever the box
 * applied before. */
void device_config_mark_seeded(uint32_t revision, const device_config_state_t *snapshot);
/* The account keeps serving revision, below the applied one (its numbering
 * restarted, or a bench apply ran ahead of it): the box takes the account's
 * numbering. With a local change pending the applied revision becomes
 * revision, so the write-back (If-Match revision) carries the change; without
 * one it drops below revision, so the account's document applies as newer.
 * Returns whether a local change is pending. */
bool device_config_rebase(uint32_t revision);
/* Firmware policy of the last accepted document. */
void device_config_firmware(device_config_firmware_t *out);
/* Feed popup policy (defaults when no document named one). */
void device_config_feed(device_config_feed_t *out);
/* True when local minute-of-day lies in the quiet hours (from inclusive, to
 * exclusive; a range over midnight wraps). */
bool device_config_feed_quiet_now(const device_config_feed_t *feed, int minute_of_day);
/* Forget or new pairing: next document applies whatever its revision. */
void device_config_reset(void);
void device_config_get_state(device_config_state_t *out);
/* Desired part from the device's current values (for the write-back body). */
size_t device_config_local_json(char *out, size_t capacity);
/* Local values as the write-back body: local_json plus the firmware member
 * the account holds (echoed so a write-back never resets it). */
size_t device_config_writeback_json(char *out, size_t capacity);
/* The check-in "reported" object (wake, assist, audio, locale, offered
 * values) without a wrapper. */
size_t device_config_reported_body(char *out, size_t capacity);
/* Reported part: firmware, offered phrases and the effective wake state, so
 * the settings page offers exactly what this box can run. */
size_t device_config_reported_json(char *out, size_t capacity);
/* KBX config?: state, local and reported documents. */
void device_config_log_status(void);
const char *device_config_outcome_name(device_config_outcome_t outcome);

#ifdef __cplusplus
}
#endif
