#pragma once
#include <stdbool.h>
#include <stddef.h>

#define KBX_CLOUD_SERVER "https://api.kombify.io/v1/speechkit"

bool kbx_cloud_server(const char *base);
bool kbx_session_url(const char *base, const char *session_id, bool websocket,
                     char *out, size_t capacity);
