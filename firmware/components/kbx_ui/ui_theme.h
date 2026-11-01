/*
 * kombify box - UI 2 colour tokens (RGB565 panel). Plain sRGB hex, no LVGL
 * types, so the theme can be included anywhere. Source: the `:root` tokens
 * of the UI 2 mockups (kombify finish: opaque surfaces,
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

/* Voice-state feedback, one set of values for every board.
 * Idle: the kombify mark keeps IDLE_MARK_SAT_PCT of its colour (the rest is
 * mixed toward IDLE_MARK_GREY) at IDLE_MARK_BRIGHT_PCT of its brightness;
 * wake, listening, thinking and speaking take full colour over
 * STATE_FADE_MS. On the 1-bit RLCD the same values land in the dither band
 * (dithered idle, solid active). */
#define KBX_IDLE_MARK_SAT_PCT      45
#define KBX_IDLE_MARK_BRIGHT_PCT   62
#define KBX_IDLE_MARK_GREY         0x8C8C8C
#define KBX_STATE_FADE_MS          260

/* Reply overlay: while a recognized or spoken sentence is shown, the orb
 * sits behind a ground-coloured scrim of REPLY_SCRIM_PCT opacity and that
 * one sentence (never a log) is drawn in front, in the 28 px face for Latin
 * locales (kbx_lat_28) and the 20 px locale face elsewhere. */
#define KBX_REPLY_SCRIM_PCT        80
#define KBX_REPLY_SCRIM_PCT_MONO   55   /* 1-bit panel: a fainter scrim keeps a dithered orb */
#define KBX_REPLY_FONT_PX          28
#define KBX_REPLY_FONT_FALLBACK_PX 20
