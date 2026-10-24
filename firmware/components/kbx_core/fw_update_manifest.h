#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Board id per image (Kconfig KBX_BOARD); host tests build without sdkconfig
 * and get the 1.85C id. An image refuses a manifest for another board. */
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif
#ifdef CONFIG_KBX_BOARD_ID
#define FW_UPDATE_BOARD CONFIG_KBX_BOARD_ID
#else
#define FW_UPDATE_BOARD "waveshare-esp32s3-touch-lcd-1.85"
#endif
/* Release asset names carry the board; the 1.85C keeps its original names. */
#if defined(CONFIG_KBX_BOARD_ID) && !CONFIG_KBX_OTA_LEGACY_ASSET_NAMES
#define FW_UPDATE_ASSET_SUFFIX "-" FW_UPDATE_BOARD
#else
#define FW_UPDATE_ASSET_SUFFIX ""
#endif
/* One publisher authority, with no credential-bearing URLs or redirects. */
#ifndef FW_UPDATE_BASE_URL
#define FW_UPDATE_BASE_URL "https://github.com/kombifyio/Box/releases"
#endif
#define FW_UPDATE_MANIFEST_URL FW_UPDATE_BASE_URL "/latest/download/box-firmware" FW_UPDATE_ASSET_SUFFIX ".json"

typedef struct {
    char version[32];
    char source_sha[41];
    uint8_t sha256[32];
    uint8_t elf_sha256[32];
    size_t size;
} fw_update_manifest_t;

/* Untrusted manifest: exact schema, board, digests and bounded size required. */
bool fw_update_manifest_parse(const char *json, fw_update_manifest_t *out);
bool fw_update_manifest_url(const fw_update_manifest_t *manifest, char *url, size_t size);
/* Manifest URL of one release ("0.31.0"): only the version token is caller
 * data, restricted to the characters a manifest version may carry. NULL or
 * empty selects the latest release. The manifest stays the only image
 * authority (board, version, size, SHA-256, ELF digest). */
bool fw_update_manifest_version_url(const char *version, char *url, size_t size);
bool fw_update_version_valid(const char *version);
bool fw_update_release_url_allowed(const char *url);
