#include "session_url.h"
#include <stdio.h>
#include <string.h>

bool kbx_cloud_server(const char *base)
{
    return base && strcmp(base, KBX_CLOUD_SERVER) == 0;
}

bool kbx_session_url(const char *base, const char *sid, bool websocket,
                     char *out, size_t capacity)
{
    if (!base || !out || !capacity) return false;
    const char *host;
    const char *scheme;
    if (strncmp(base, "https://", 8) == 0) {
        host = base + 8;
        scheme = websocket ? "wss://" : "https://";
    } else if (strncmp(base, "http://", 7) == 0 && !kbx_cloud_server(base)) {
        host = base + 7;
        scheme = websocket ? "ws://" : "http://";
    } else return false;
    if (!*host || strpbrk(host, "?#@\\\r\n ")) return false;
    if (sid) {
        size_t length = strlen(sid);
        if (length == 0 || length > 63) return false;
        for (const char *p = sid; *p; ++p) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
        }
    } else if (websocket) return false;
    const char *prefix = kbx_cloud_server(base) ? "/voiceagent/sessions" : "/v1/voiceagent/sessions";
    int written = snprintf(out, capacity, "%s%s%s%s%s%s", scheme, host, prefix,
                           sid ? "/" : "", sid ? sid : "", websocket ? "/ws" : "");
    if (written < 0 || (size_t)written >= capacity) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static bool edge_device_id(const char *id)
{
    if (!id || strncmp(id, "dev_", 4) != 0) return false;
    size_t n = 0;
    for (const char *p = id + 4; *p; ++p, ++n) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    }
    return n == 43;
}

bool kbx_mint_body(const char *fields, const char *device_id, char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    if (!fields) fields = "";
    bool device = edge_device_id(device_id);
    int written = snprintf(out, capacity, "{%s%s%s%s%s}", fields,
                           device && *fields ? "," : "",
                           device ? "\"device_id\":\"" : "", device ? device_id : "",
                           device ? "\"" : "");
    if (written < 0 || (size_t)written >= capacity) {
        out[0] = '\0';
        return false;
    }
    return true;
}
