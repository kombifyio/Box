/*
 * kombify box - lan_pair: find a homelab speechkit-server on the LAN and pair
 * with it (speechkit.pairing.v2, SpeechKit docs/server/pairing.md).
 *
 * Discovery browses DNS-SD `_speechkit._tcp` and takes the `url` of an
 * instance whose `modes` include `voiceagent`; an announcement that carries a
 * credential-like TXT key is ignored whole. Pairing asks the server for this
 * Box's own revocable device token: the Box shows the server's 6-digit code,
 * the operator approves that code (`speechkit-server pairing approve <code>`
 * or /setup), the Box collects the token once and stores it as the net_ws
 * bearer (NVS `net`/`sk_token`). The shared server bearer never reaches the
 * Box. Nothing found, refused, denied or expired: the Box stays unpaired and
 * shows why.
 *
 * CDC (usb_log.c): `KBX server auto`, `KBX lan pair`, `KBX lan cancel`,
 * `KBX lan?`.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAN_PAIR_IDLE = 0,
    LAN_PAIR_BROWSING,     /* DNS-SD query running                        */
    LAN_PAIR_REQUESTING,   /* POST /v1/pairing/device-requests            */
    LAN_PAIR_SHOW_CODE,    /* code on the display, polling for approval   */
    LAN_PAIR_PAIRED,       /* device token stored                         */
    LAN_PAIR_FAILED,       /* error holds the reason                      */
} lan_pair_phase_t;

typedef struct {
    lan_pair_phase_t phase;
    char code[8];          /* six digits while SHOW_CODE                  */
    char server[96];       /* host[:port] of the chosen server            */
    char instance[64];     /* DNS-SD instance name, when discovered       */
    int expires_in;        /* seconds left for the code                   */
    char error[32];        /* offline, no_server, pairing_off, not_lan,
                              denied, expired, busy, server_error, ...    */
    uint32_t seq;          /* changes on every status change              */
} lan_pair_status_t;

/* After net_wifi_init and net_ws_init. Boot rule: no server stored (NVS
 * `net`/`sk_url` empty) -> discover and pair; a LAN server stored without a
 * token -> pair with it. A Cloud-profile Box (or one in Cloud pairing) is
 * left alone. Runs on its own task. */
esp_err_t lan_pair_init(void);

/* discover: browse `_speechkit._tcp` and switch to the found server (a
 * different server clears the old URL and token first). pair: request a new
 * device token even when one is stored. Cancels a running attempt. */
esp_err_t lan_pair_start(bool discover, bool pair);
void lan_pair_cancel(void);
void lan_pair_get_status(lan_pair_status_t *out);
/* "lan_pair: status: phase=… server=… code=… error=…"; never a secret. */
void lan_pair_log_status(void);

#ifdef __cplusplus
}
#endif
