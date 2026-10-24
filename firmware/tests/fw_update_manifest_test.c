/* OTA authority boundary: malformed metadata must never authorize a flash. */
#include "../components/kbx_core/fw_update_manifest.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char valid[] = "{\"schema_version\":1,\"board\":\"" FW_UPDATE_BOARD "\","
    "\"version\":\"0.40.0\",\"source_sha\":\"0123456789abcdef0123456789abcdef01234567\","
    "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"elf_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"size\":4096}";

static void reject(const char *key, const char *replacement)
{
    cJSON *json = cJSON_Parse(valid);
    cJSON_ReplaceItemInObjectCaseSensitive(json, key, cJSON_Parse(replacement));
    char *encoded = cJSON_PrintUnformatted(json);
    fw_update_manifest_t out;
    assert(!fw_update_manifest_parse(encoded, &out));
    free(encoded); cJSON_Delete(json);
}

int main(void)
{
    fw_update_manifest_t out;
    assert(fw_update_manifest_parse(valid, &out));
    assert(out.size == 4096 && out.sha256[31] == 0xaa && out.elf_sha256[31] == 0xbb);
    char url[512];
    assert(fw_update_manifest_url(&out, url, sizeof(url)));
    assert(!strcmp(url, FW_UPDATE_BASE_URL "/download/v0.40.0/kombify_box" FW_UPDATE_ASSET_SUFFIX "-0123456789abcdef0123456789abcdef01234567.bin"));
    assert(!fw_update_manifest_url(&out, url, 8));
    /* A pinned release only widens the version token of the manifest URL. */
    assert(fw_update_manifest_version_url("0.31.0", url, sizeof(url)));
    assert(!strcmp(url, FW_UPDATE_BASE_URL "/download/v0.31.0/box-firmware" FW_UPDATE_ASSET_SUFFIX ".json"));
    assert(fw_update_manifest_version_url(NULL, url, sizeof(url)) && !strcmp(url, FW_UPDATE_MANIFEST_URL));
    assert(fw_update_release_url_allowed(url));
    assert(!fw_update_manifest_version_url("0.31.0/../x", url, sizeof(url)));
    assert(!fw_update_manifest_version_url("0.31.0?x=1", url, sizeof(url)));
    assert(!fw_update_manifest_version_url("0.31.0#a", url, sizeof(url)));
    assert(fw_update_release_url_allowed(FW_UPDATE_MANIFEST_URL));
    assert(fw_update_release_url_allowed("https://release-assets.githubusercontent.com/github-production-release-asset/asset?signature=public"));
    assert(!fw_update_release_url_allowed("http://github.com/kombifyio/Box/releases/latest"));
    /* The former SpeechKit release channel is no longer an image authority. */
    assert(!fw_update_release_url_allowed("https://github.com/kombifyio/SpeechKit/releases/latest/download/box-firmware.json"));
    assert(!fw_update_release_url_allowed("https://github.com@evil.test/kombifyio/Box/releases/latest"));
    assert(!fw_update_release_url_allowed("https://github.com/kombifyio/Other/releases/latest"));
    assert(!fw_update_release_url_allowed("https://release-assets.githubusercontent.com.evil.test/file"));
    assert(!fw_update_release_url_allowed("https://release-assets.githubusercontent.com:443/file"));
    assert(!fw_update_release_url_allowed("https://github.com/kombifyio/Box/releases/../../Other/releases/latest"));
    assert(!fw_update_release_url_allowed("https://release-assets.githubusercontent.com/file\r\nOther: x"));
    reject("schema_version", "2");
    reject("board", "\"other-board\"");
    reject("size", "0"); reject("size", "6291457"); reject("size", "4096.5");
    reject("size", "\"4096\""); reject("size", "1e100");
    reject("sha256", "\"aa\"");
    reject("sha256", "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag\"");
    reject("source_sha", "\"../other-repo\"");
    reject("version", "\"0.40.0/../../other\"");
    reject("version", "\"0.40.0?x=other\"");
    reject("version", "\"\"");
    char ambiguous[2200];
    snprintf(ambiguous, sizeof(ambiguous), "{\"board\":\"other\",%s", valid + 1);
    assert(!fw_update_manifest_parse(ambiguous, &out));
    snprintf(ambiguous, sizeof(ambiguous), "%s trailing", valid);
    assert(!fw_update_manifest_parse(ambiguous, &out));
    snprintf(ambiguous, sizeof(ambiguous), "{\"version\":\"0.40.0\\u0000/other\",%s", valid + 1);
    assert(!fw_update_manifest_parse(ambiguous, &out));
    puts("OTA manifest authority boundary passed");
    return 0;
}
