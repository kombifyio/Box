#include "fw_update_manifest.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static bool hex(const char *s, size_t n, uint8_t *bytes)
{
    if (!s || strlen(s) != n) return false;
    unsigned value = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else return false;
        value = (value << 4) | digit;
        if (bytes && i % 2) bytes[i / 2] = (uint8_t)value;
    }
    return true;
}

static const cJSON *field(const cJSON *root, const char *name)
{
    const cJSON *found = NULL;
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (item->string && !strcmp(item->string, name)) {
            if (found) return NULL;
            found = item;
        }
    }
    return found;
}

bool fw_update_manifest_parse(const char *json, fw_update_manifest_t *out)
{
    if (!json || !out || strlen(json) > 2048 || strstr(json, "\\u0000")) return false;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    const cJSON *schema = field(root, "schema_version");
    const cJSON *board = field(root, "board");
    const cJSON *version = field(root, "version");
    const cJSON *source = field(root, "source_sha");
    const cJSON *sha = field(root, "sha256");
    const cJSON *elf = field(root, "elf_sha256");
    const cJSON *size = field(root, "size");
    fw_update_manifest_t parsed = {0};
    bool ok = cJSON_IsNumber(schema) && schema->valuedouble == 1 &&
        cJSON_IsString(board) && !strcmp(board->valuestring, FW_UPDATE_BOARD) &&
        cJSON_IsString(version) && version->valuestring[0] && strlen(version->valuestring) < sizeof(parsed.version) &&
        cJSON_IsString(source) && hex(source->valuestring, 40, NULL) &&
        cJSON_IsString(sha) && hex(sha->valuestring, 64, parsed.sha256) &&
        cJSON_IsString(elf) && hex(elf->valuestring, 64, parsed.elf_sha256) &&
        cJSON_IsNumber(size) && size->valuedouble >= 1024 && size->valuedouble <= 6 * 1024 * 1024 &&
        size->valuedouble == (double)(size_t)size->valuedouble;
    if (ok) {
        for (const char *p = version->valuestring; *p; ++p)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= 'A' && *p <= 'Z') || *p == '.' || *p == '-' || *p == '+')) ok = false;
    }
    if (ok) {
        strcpy(parsed.version, version->valuestring);
        strcpy(parsed.source_sha, source->valuestring);
        parsed.size = (size_t)size->valuedouble;
        *out = parsed;
    }
    cJSON_Delete(root);
    return ok;
}

bool fw_update_manifest_url(const fw_update_manifest_t *manifest, char *url, size_t size)
{
    if (!manifest || !url || !hex(manifest->source_sha, 40, NULL)) return false;
    int n = snprintf(url, size, FW_UPDATE_BASE_URL "/download/v%s/kombify_box" FW_UPDATE_ASSET_SUFFIX "-%s.bin", manifest->version, manifest->source_sha);
    return n > 0 && (size_t)n < size;
}

bool fw_update_version_valid(const char *version)
{
    if (!version || !version[0] || strlen(version) >= sizeof(((fw_update_manifest_t *)0)->version)) return false;
    for (const char *p = version; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') || *p == '.' || *p == '-' || *p == '+')) return false;
    return true;
}

bool fw_update_manifest_version_url(const char *version, char *url, size_t size)
{
    if (!url || !size) return false;
    int n;
    if (!version || !version[0]) {
        n = snprintf(url, size, "%s", FW_UPDATE_MANIFEST_URL);
    } else {
        if (!fw_update_version_valid(version)) return false;
        n = snprintf(url, size, FW_UPDATE_BASE_URL "/download/v%s/box-firmware" FW_UPDATE_ASSET_SUFFIX ".json", version);
    }
    return n > 0 && (size_t)n < size;
}

bool fw_update_release_url_allowed(const char *url)
{
    /* The release channel is FW_UPDATE_BASE_URL and nothing else. */
    static const char github[] = FW_UPDATE_BASE_URL "/";
    static const char assets[] = "https://release-assets.githubusercontent.com/";
    if (!url || strstr(url, "/../") || strstr(url, "/./") || strchr(url, '#')) return false;
    for (const char *p = url; *p; ++p) if ((unsigned char)*p <= 32 || *p == '\\') return false;
    return strncmp(url, github, sizeof(github) - 1) == 0 ||
           strncmp(url, assets, sizeof(assets) - 1) == 0;
}
