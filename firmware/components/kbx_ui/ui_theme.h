/*
 * kombify box - UI 2 colour tokens (RGB565 panel). Plain sRGB hex, no LVGL
 * types, so the theme can be included anywhere. Source: the `:root` tokens
 * of the UI mockups (kombify finish: opaque surfaces,
 * hairline borders, no blur). State colours of the orb live in aura_orb.c
 * (speechkit.voice_ui.v1 tokens.json), never here.
 */
#pragma once

/* surfaces */
#define KBX_COL_BG          0x0B0E14  /* screen ground                     */
#define KBX_COL_SURF        0x121826  /* tiles, sheets                     */
#define KBX_COL_SURF2       0x182033  /* raised control inside a surface   */
#define KBX_COL_LINE        0x243049  /* hairline border                   */
#define KBX_COL_PILL        0x141B28  /* status pill fill                  */
#define KBX_COL_RING_TRACK  0x1A2233  /* edge ring track                   */

/* brand (kombify teal) */
#define KBX_COL_BRAND       0x45AEBB
#define KBX_COL_BRAND_DEEP  0x164C54
#define KBX_COL_TINT        0x0F2A2E  /* brand-tinted surface              */
#define KBX_COL_TINT_LINE   0x2A6B74  /* its hairline                      */

/* alert */
#define KBX_COL_ALERT       0xF87171  /* alert text / icon                 */
#define KBX_COL_ALERT_RING  0xD84A4A  /* mute edge ring                    */
#define KBX_COL_ALERT_BG    0x24141A
#define KBX_COL_ALERT_LINE  0x5A2A2E

/* text */
#define KBX_COL_TEXT_HI     0xEDEFF5  /* primary                           */
#define KBX_COL_TEXT_MID    0xC7D0DA  /* chips, pills                      */
#define KBX_COL_TEXT_LO     0x8A93A6  /* secondary (6.3:1 on the ground)   */
