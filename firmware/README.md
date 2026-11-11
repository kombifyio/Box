# kombify box - Firmware

Turns the Waveshare **ESP32-S3-Touch-LCD-1.85C** into the USB audio device
**"kombify box"** (UAC: mic 1ch + speaker 2ch @ 48 kHz) with a status UI on
the round display (ready / speaking / mute, mic-level ring).

## Hardware mapping (verified from Waveshare wiki + xiaozhi board support)

| Function | Pins |
|---|---|
| Shared I2S (ES7210 ADC + ES8311 DAC) | MCLK 2, BCLK 48, LRCK 38, DOUT 47, DIN 39; PA 15 |
| LCD ST77916 QSPI | PCLK 40, CS 21, D0-3 46/45/42/41, BL 5 |
| LCD/Touch reset | TCA9554 (I2C 10/11) EXIO0+EXIO1 |
| Keep-alive power | GPIO7 high, PWR button GPIO6, BOOT GPIO0 |

There are two panel revisions; the firmware reads register 0x04 and picks
the init sequence automatically (same as the Waveshare/xiaozhi reference).

## Second board: Waveshare ESP32-S3-RLCD-4.2

One source tree, one image per board: Kconfig `KBX_BOARD` selects a board
profile in `main/Kconfig.projbuild`. The profile sets capability symbols for
the panel driver, input, display shape/colour, audio capture, sensors and
home screen; C and CMake branch on those capabilities. Only `board.h` (`components/kbx_board`)
selects a pin/tuning header by board identity. `CONFIG_KBX_BOARD_ID` supplies
the OTA identity. The profiles and their capabilities are declared in
[`main/Kconfig.projbuild`](main/Kconfig.projbuild).

| Function | RLCD 4.2 |
|---|---|
| Display | ST7305 reflective 400x300, 1 bpp, no backlight; SPI SCLK 11, MOSI 12, CS 40, DC 5, RST 41 (`components/kbx_display/display_rlcd.c`) |
| Audio | ES7210 + ES8311 as on the 1.85C V2; I2S MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10, PA 46 (high = on); external 8 Ω speaker on the MX1.25 header |
| I2C | SDA 13, SCL 14: ES8311 0x18, ES7210 0x40, PCF85063 0x51, SHTC3 0x70 |
| Buttons (no touch) | KEY GPIO18: tap = talk, hold = mute. BOOT GPIO0: tap = volume, hold = account pairing QR. PWR is hardware-only (load switch, no GPIO, no keep-alive pin) |

The display renders the UI 2 theme in RGB565 and converts it to 1 bit:
inverted luminance (dark ground = paper) with a narrow Bayer band, so text
is solid ink. Pins and the ST7305 init sequence come from
`waveshareteam/ESP32-S3-RLCD-4.2` (07_Audio_Test) and its schematic.

```powershell
idf.py -B build-rlcd42 -D SDKCONFIG=sdkconfig.rlcd42 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.rlcd42" build
# bench build with KBX sync put: append ;sdkconfig.defaults.dev to SDKCONFIG_DEFAULTS
```

Its release assets carry the board id (`box-firmware-waveshare-esp32s3-rlcd-4.2.json`,
`kombify_box-waveshare-esp32s3-rlcd-4.2-<sha>.bin`); the 1.85C keeps its
original names. The CI builder produces both boards in one artifact.

## Build (Windows)

1. Install **ESP-IDF v6.0.3**: https://dl.espressif.com/dl/esp-idf/
   (offline installer; target esp32s3 is enough).
2. Open "ESP-IDF 6.0 PowerShell", then:
   ```powershell
   cd <this directory>
   idf.py set-target esp32s3
   idf.py build
   ```
   Third-party packages (LVGL, TinyUSB/UAC, LCD/Touch, IO expander) are
   pinned exactly in `main/idf_component.yml` and are loaded from the
   Component Manager into `managed_components/` on the first build — that
   needs one-time registry access. `dependencies.lock` pins the resolved
   version including transitives and is committed. The only exception:
   `components/usb_device_uac/` is local because it patches the CDC log
   port (see its KOMBIFY-PATCH.md).
3. **Flash.** App firmware runs as TinyUSB VID `303A` PID `8000` (CDC, often COM5); ROM download is PID `1001` (often COM4); USB-Serial/JTAG PID `0009` is **not** the loader. Update without the BOOT button:
   ```powershell
   powershell -ExecutionPolicy Bypass -File ..\scripts\ota-push.ps1
   ```
   The script streams `kombify_box.bin` over CDC into the inactive OTA slot; image bytes must wait for the log line `kbx_ota: recv`, otherwise the CDC parser overflows and the COM port wedges. `KBX ota http://.../kombify_box.bin` pulls the image over Wi-Fi. SHA verification (`esp-sha`) needs internal heap and can fail with `Failed to allocate buf memory` (heap_int ~2.6-3.5 KB) under Wi-Fi/voice; the firmware pauses voice first, and if it still fails use software download mode, not another CDC OTA.

   Full esptool flash (partition table, rollback image, restore): `KBX bootloader` starts the ROM loader (PID `1001`) **without** USB persist; wait 1-2 s, then `firmware/reflash-com.ps1` or `idf.py -p COMx flash` on the download port. `RTC_CNTL_FORCE_DOWNLOAD_BOOT` (`0x60008128`) survives a watchdog while the RTC battery holds; if the box sticks in PID `1001`, write it to 0 before `esp_restart`.

   Current device image and its accepted evidence: [`docs/device-baseline.md`](../docs/device-baseline.md).

## Voice-state feedback and reply overlay

Both S3 boards (and the C6 AMOLED tree, which includes the same header) take
their values from `components/kbx_ui/ui_theme.h`:

- **Idle vs active.** The kombify mark rests desaturated and dimmed
  (`KBX_IDLE_MARK_SAT_PCT`, `KBX_IDLE_MARK_BRIGHT_PCT`, mixed toward
  `KBX_IDLE_MARK_GREY`) and takes full colour while the box wakes, listens,
  thinks or speaks, eased over `KBX_STATE_FADE_MS` (`aura_orb.c`). On the 1-bit
  RLCD the same values land in the dither band: dithered when idle (also the
  small mark on the home screen), solid when active.
- **Reply overlay.** While the user's recognized sentence or the assistant's
  current sentence is shown (only that sentence, never a log), the orb sits
  behind a ground-coloured scrim (`KBX_REPLY_SCRIM_PCT`) and the sentence is
  drawn in front in the 28 px face for Latin locales
  (`KBX_REPLY_FONT_PX`, stepping down to the 20 and 16 px locale faces when a
  long sentence needs it; `KBX_REPLY_FONT_FALLBACK_PX` is the locale face for
  scripts without a 28 px set). Round: a centred label over the orb; RLCD: the
  existing chat bubble (user solid, agent outlined).

## Display design (classic or watch)

Two designs live in every image and the account picks one per device
(`display.design` in `speechkit.box_settings.v1`, Techstack > device settings >
Display; `KBX design classic|watch` on the console):

- **classic**, the default of the round 1.85C and the RLCD 4.2: the layouts
  above (round: a 180 px orb with caption and pill; RLCD: room display with a
  small mark while idle, solid orb in a turn).
- **watch**, the default of the C6 AMOLED: the Pixel Watch layout. The orb is
  the face (288 px on the round board, 216 px on the RLCD, the mark 0.58 of
  it) and the secondary information sits in a wider rim: time, Wi-Fi and
  battery (where a gauge exists) on the top chord of the round board, a state
  pill on the bottom chord; on the RLCD two solid ink rims with date, time and
  status symbols on top and the room climate (or the state pill) below.

The screen is built once at start (`kbx_design_init`, `display_design.c`); a
change is stored in NVS and the box restarts into it as soon as it has been out
of a conversation for 4 s, so no reflash is needed. The profiles are in
`display_profile.c`, the per-design branches in `ui_scene.c` (`s_watch`),
`ui_home.c` (classic RLCD) and `ui_home_watch.c` (RLCD rims).

## Feed popups

A new Feed item of the account that paired the box pops up on the display
(ai-platform-1vu2): the kind icon (household, homelab, paperwork, agents,
system), the subject and "+N" when several arrived, for 30 s or until a tap,
the key or Close dismisses it, with the soft listening chime unless the box is
muted. The sync client reads `GET /v1/connect/edge-devices/<dev>/feed` on the
connection of a check-in or settings poll (`edge_sync.c`, no extra TLS
session); Gateway returns at most five unread items as subject, kind, icon and
time, never a body or link. The account switches popups per device with
`feed.popup` in `speechkit.box_settings.v1` (default on) and holds them during
`feed.quiet_from`/`feed.quiet_to` (box local time); held items fold into one
popup afterwards. After boot or pairing only items from the last two minutes
pop up; older ones stay in the Feed. `KBX sync?` logs the feed state.

`KBX shot` (round board, `tools/s3-screenshot.py COM4 out.png`) streams the
screen over the CDC; the RLCD frame is read over the LAN console
(`scripts/kbx-remote.ps1 -Screenshot`).

## After flashing

Windows > Sound: recording and playback device **"kombify box"**. The display ring pulses with mic level and turns green during TTS playback. Wakeword work follows the constraints in [`docs/device-baseline.md`](../docs/device-baseline.md), not a TFLM retry on the running box.

## Display language (i18n)

All customer-visible display text is a message ID resolved by `i18n_str()`
(`components/kbx_core/include/i18n.h`). Supported: **en** (default), **de**, **es**, **zh-Hans**,
**ar**. Set with `KBX locale <tag>` over the USB CDC channel (`KBX locale?`
reports it), or push an optional `locale` field in the session-create response;
the tag is stored in NVS (`kbx_ui/locale`) and applied live.

- Source of truth: `i18n/messages.json` (en, de du, es tu, zh-Hans, ar MSA, hi).
  Regenerate the C table with `node firmware/i18n/gen.mjs`, and the LVGL font
  subsets with `node firmware/i18n/gen.mjs --fonts` (pins and runs
  `lv_font_conv` with `--symbols` for exactly the glyphs the strings use;
  the commands are printed). Commit the generated `components/kbx_core/` i18n files and
  `components/kbx_ui/assets/fonts/kbx_*.c`.
- Fonts: Montserrat plus accented letters (de/es), Noto Sans SC subset (zh),
  Noto Sans Arabic presentation forms (ar). LVGL Arabic shaping and BiDi are
  enabled in `sdkconfig.defaults`; Arabic labels use an RTL base direction.
- **Hindi (hi) limitation:** LVGL's text engine cannot shape Devanagari
  (no conjuncts, no pre-base i-matra reordering). `hi` is accepted and
  stored, but the display renders English. Proper Hindi needs a shaping engine
  (HarfBuzz via FreeType) that does not fit this device's RAM budget.
- Flash cost: about 44 KB of the 6 MB OTA slot (fonts ~35 KB, tables ~1.5 KB,
  code + BiDi/Arabic shaping the rest).

The host UI preview compiles the real LAN-pair overlay (`ui_lan_pair.c`) alongside
the other UI sources; its host fixture supplies the disconnected pairing status.
