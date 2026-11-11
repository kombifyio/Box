#pragma once
#include <stdbool.h>
#include <stddef.h>

#define KBX_CLOUD_SERVER "https://api.kombify.io/v1/speechkit"

bool kbx_cloud_server(const char *base);
bool kbx_session_url(const char *base, const char *session_id, bool websocket,
                     char *out, size_t capacity);

/* Session-create body: `{<fields>}`, plus `"device_id":"dev_..."` when
 * device_id is this board's edge device id (dev_ + 43 base64url characters,
 * the id it checks in with on /v1/connect/edge-devices). The Gateway signs it
 * into the SpeechKit edge envelope, so the session counts against this board,
 * never against the account's other boards. fields is a
 * JSON member list without braces, or "". Returns false when out is too small. */
bool kbx_mint_body(const char *fields, const char *device_id, char *out, size_t capacity);
