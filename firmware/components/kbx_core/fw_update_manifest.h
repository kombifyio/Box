#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Board id this image was built for (Kconfig KBX_BOARD); host tests build
 * without sdkconfig and get the 1.85C id. Update selection and acceptance
 * follow the board the hardware probe names (kbx_board_effective_id()),
 * passed in as `board`; FW_UPDATE_BOARD is only the compiled default. */
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
#define FW_UPDATE_LEGACY_BOARD "waveshare-esp32s3-touch-lcd-1.85"
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
#define FW_UPDATE_BOARD_MAX 48

typedef struct {
    char board[FW_UPDATE_BOARD_MAX];
    char version[32];
    char source_sha[41];
    uint8_t sha256[32];
    uint8_t elf_sha256[32];
    size_t size;
} fw_update_manifest_t;

/* Untrusted manifest: exact schema, board, digests and bounded size required.
 * _board takes the board the manifest must name; the plain one the compiled. */
bool fw_update_manifest_parse_board(const char *json, const char *board, fw_update_manifest_t *out);
bool fw_update_manifest_parse(const char *json, fw_update_manifest_t *out);
/* Image URL of a parsed manifest, named for the manifest's board. */
bool fw_update_manifest_url(const fw_update_manifest_t *manifest, char *url, size_t size);
/* Manifest URL of one release ("0.31.0"): only the version token is caller
 * data, restricted to the characters a manifest version may carry. NULL or
 * empty selects the latest release. The manifest stays the only image
 * authority (board, version, size, SHA-256, ELF digest). */
bool fw_update_manifest_version_url_board(const char *board, const char *version, char *url, size_t size);
bool fw_update_manifest_version_url(const char *version, char *url, size_t size);
bool fw_update_version_valid(const char *version);
bool fw_update_release_url_allowed(const char *url);

/* Board marker every image carries right after esp_app_desc_t (section
 * .rodata_custom_desc): image header 24 + segment header 8 + app desc 256
 * bytes. The OTA paths read it from the first bytes of an incoming image and
 * refuse another board's image before the inactive slot is touched. */
#define FW_IMAGE_BOARD_MAGIC 0x4258424Bu   /* "KBXB" in image byte order */
#define FW_IMAGE_BOARD_VERSION 1u
#define FW_IMAGE_BOARD_OFFSET 288u
typedef struct {
    uint32_t magic;
    uint32_t version;
    char board[56];
} fw_image_board_t;
#define FW_IMAGE_HEAD_SIZE (FW_IMAGE_BOARD_OFFSET + sizeof(fw_image_board_t))

/* Board id of an application image from its first bytes. False when the
 * bytes are no ESP application image or carry no valid marker (images built
 * before the marker existed). */
bool fw_update_image_board(const uint8_t *head, size_t len, char *board, size_t size);
