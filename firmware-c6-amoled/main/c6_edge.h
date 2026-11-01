/*
 * kombify box C6 - edge device management.
 *
 * The C6 lists itself in the owner's device list and takes the account's
 * settings document like the S3 boards (firmware/components/kbx_core/
 * edge_sync.c): same routes, same device id derivation, same document
 * (speechkit.box_settings.v1), pull only. Contract: Connect contract
 * section 9.2 (edge devices).
 *
 *   PUT  /v1/connect/edge-devices/<device_id>            check-in
 *   GET  /v1/connect/edge-devices/<device_id>/settings   If-None-Match
 *   PUT  /v1/connect/edge-devices/<device_id>/settings   If-Match
 *
 * Without PSRAM there is no second TLS stack: a cycle runs inside the voice
 * task's idle upkeep, on the voice TLS transport, while the wake engine is
 * parked, and only when the box is idle. The C6 applies the members it has
 * (wake, assist.max_followups, audio.volume, locale) and ignores the rest
 * (sound cues, voice, route, firmware: no such feature on this board).
 */
#pragma once
#include <stdbool.h>

/* After NVS and c6_voice_init: loads the sync state. */
void c6_edge_init(void);
/* True when a check-in, a settings poll or a write-back is due. */
bool c6_edge_due(void);
/* Ask for a cycle at the next idle upkeep (KBX sync now). */
void c6_edge_poke(void);
/* One cycle. bearer is the account access token (JWT, no "Bearer "). Voice
 * task only, with the wake engine parked. */
void c6_edge_run(const char *bearer);
/* How long the voice task may sleep before the next poll is due, at most max_ms. */
unsigned c6_edge_wait_ms(unsigned max_ms);
void c6_edge_log_status(void);
