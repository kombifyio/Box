# kombify box C6 AMOLED

Cloud voice satellite firmware for the Waveshare **ESP32-C6-Touch-AMOLED-1.8**:
the third kombify box device beside the round S3
Box and the RLCD 4.2 board (both `firmware/`). Same standalone
Cloud profile as the S3 Box: account pairing by phone QR, tap to talk, one
bounded voice session per tap through the Gateway, no provider keys and no
PC at runtime.

## Hardware

| Part | Detail |
| --- | --- |
| SoC | ESP32-C6 (QFN40, rev v0.2), 160 MHz single-core RISC-V, Wi-Fi 6 2.4 GHz, BLE 5, 802.15.4; **no PSRAM** |
| Flash | 16 MB (XMC, ID `0x204018`) |
| Display | 1.8" AMOLED 368x448, SH8601 over QSPI (variant V1); V2 boards use CO5300, detected at runtime |
| Touch | FT3168 at `0x38` (V1), CST820 at `0x15` (V2) |
| Audio | ES8311 codec, onboard mic and speaker; amplifier on TCA9554 pin 7 |
| Other | TCA9554 expander `0x20`, AXP2101 PMU `0x34`, QMI8658 IMU `0x6B`, PCF85063 RTC `0x51`, microSD on SPI |
| USB | built-in USB-Serial/JTAG (VID `303A` PID `1001`), console and flashing |

Pins and drivers come from Waveshare's board package
`waveshare/esp32_c6_touch_amoled_1_8` 1.0.0. The factory image was Espressif's
ESP-Brookesia phone demo (drawing, recorder and music apps).

## What differs from the S3 Box

| Concern | S3 Box | C6 AMOLED |
| --- | --- | --- |
| Memory | 8 MB PSRAM | 512 KB SRAM only |
| Turn | full-duplex, capture starts at the tap into a PSRAM FIFO | half-duplex; capture starts at the tap or wake word into a static 32 KB IMA-ADPCM FIFO (~4 s), decoded back to PCM for the uplink |
| Audio | ES7210 + ES8311, 48 kHz USB audio | ES8311 only, I2S re-clocked per direction (16 kHz up, 24 kHz down) |
| USB | TinyUSB UAC + CDC | USB-Serial/JTAG console, no USB audio |
| UI | baked Aura Orb layers in PSRAM | same palette and rosette as plain LVGL widgets |
| Wake word | ESP-SR WakeNet9 and microWakeWord | microWakeWord only (same engine and models); unloaded during a turn |
| Conversation mode, WLAN updates | yes | not yet |

Shared without copies: `cloud_auth.c`, `net_wifi.c`, `session_url.c`,
`voice_protocol.h`, `voice_error.h`, `ui_theme.h`, the kombify rosette and the
`kbx_lat` fonts and the microWakeWord engine (`wakeword_mww.cc`) and models
from `firmware/components` and `firmware/models/mww`. The shared changes keep the S3
build identical: `cloud_auth.c` and `wakeword_mww.cc` pick internal RAM (and
smaller auth work buffers) only when `CONFIG_SPIRAM` is off,
`voice_protocol.h` lets a board pass a smaller `KBX_VOICE_TEXT_LIMIT`, and
`cloud_auth.c` takes the renewal margin from `CLOUD_AUTH_RENEW_MARGIN_S`
(default 600 s; the C6 uses 90 s, its token only mints a session).

## Latency

Tap or wake word to the server's `listening`: about 2 s once warm, against
8 to 12 s in the first variant. Three changes carry it:

- TLS: Let's Encrypt's P-384 chain costs the C6 about a second of software
  ECDSA per full handshake (only P-256 has hardware), so one TLS transport to
  `api.kombify.io` carries session create, the WSS socket and DELETE, one
  connection at a time, and resumes with the TLS 1.3 session ticket. The
  ticket is saved after a response was read, because TLS 1.3 sends it after
  the handshake. X25519 is disabled so the key share uses the P-256 hardware.
- Idle upkeep: every 15 s the idle box renews the account token when less
  than 150 s are left and refreshes the ticket hourly with one unstarted
  session (minted, dialed without `start`, deleted; nothing is billed).
- Capture starts at the trigger into the ADPCM FIFO, so the user can speak
  right after the wake word; the FIFO drains faster than real time once the
  server listens.

## Wake word

Settings and `KBX wake` select Off, Kombify, Hey Kombify, Hey Kubi or Hey
Jarvis (persisted). The default is **Hey Jarvis**: on 2026-10-22 the
kombify-trained models stayed far below their cutoffs on voices outside their
training TTS. The same clips gave identical results on the device and with
the training frontend on a PC, so the device pipeline is not the cause:

| Clip | Model | Window max | Cutoff |
| --- | --- | --- | --- |
| "Hey Jarvis", English SAPI voice | Hey Jarvis | 1.00 | 0.97 |
| "Hey Jarvis", German SAPI voice | Hey Jarvis | 0.52 | 0.97 |
| "Hey Kombify", German | Hey Kombify | 0.84 | 0.98 |
| "Hey Kombify", English | Hey Kombify | 0.10 | 0.98 |
| "Kombify", German / English | Kombify | <= 0.12 | 0.994 |
| "Hey Kubi", German / English | Hey Kubi | <= 0.08 | 0.998 |
| "Hey Kombify", German | Hey Kubi | 0.90 | 0.998 |

Inference costs about 6 ms per 30 ms window (20 % CPU, task priority above
LVGL). The engine (36 to 42 KB) is unloaded while a turn, the idle upkeep or
a microphone diagnostic runs, so the TLS handshakes keep their RAM. Wake
verification runs as a second stage in
SpeechKit (low on-device cutoff, server check before the provider starts).

The certificate bundle is copied to RAM at boot: ESP-IDF 6.0.3 reads its
16-bit fields at odd addresses, which faults from the flash cache on the C6.

## Two-stage wake word

The device side of the two-stage wake word:

- **Pre-roll:** the wake engine keeps the last 1.5 s in the capture FIFO. Every ADPCM frame carries its own codec state in a 4-byte header, so old frames can be dropped.
- **Start frame:** on a hit, the start frame carries `wake {phrase, score, preroll_ms}`, and the device streams the FIFO once the server reports `verifying`.
- **Rejection:** `wake_rejected` returns the box to idle without an error.
- **Cutoff:** the engine uses its low candidate cutoff (0.5) only after a wake turn met a verifying server, and for at most 60 candidates an hour. Before that, or with an older server, the manifest cutoff decides alone.
- **Stage-one limits:** the kombify-trained Kombify and Hey Kubi models barely rise above their baseline on unseen voices. Stage one only helps them after retraining.

## Power and screen

- **Power off:** Settings, Power off ("Ausschalten") cuts the AXP2101 rails through software power-off (REG 10h bit 0). This also works on USB power. A short press on the PWR key turns the box on again. `KBX off` does the same.
- **Screen timeout:** while idle, the AMOLED turns dark after 2 minutes without touch, which also guards against burn-in. A touch or any voice turn lights it again, and the waking touch never starts a turn.
- **Renewal memory:** idle upkeep lends the static 52 KB capture FIFO to `cloud_auth` as its renewal work area (`cloud_auth_set_work_area`). A renewal then needs only the TLS handshake from the heap. It had failed once an open settings sheet and the wake engine had fragmented the heap.

## Build and flash

The pinned toolchain is the same image as the S3 CI build:

```bash
docker build --file firmware-c6-amoled/Dockerfile.ci --output type=local,dest=firmware-c6-amoled/build/dist .
python -m esptool --chip esp32c6 -p COM7 -b 921600 write-flash @firmware-c6-amoled/build/dist/flash_args
```

`flash_args` is relative to `build/dist`; run esptool from that directory.
The first flash replaces the factory image and partition table.

## Operate

Commands go to the USB-Serial/JTAG port, one per line (open the port with
DTR and RTS low, otherwise it resets the chip):

| Command | Effect |
| --- | --- |
| `KBX wifi <ssid>\|<password>`, `KBX wifi?`, `KBX wifi clear` | Wi-Fi as on the S3 Box; the password is never echoed |
| `KBX cloud pair`, `KBX cloud?`, `KBX cloud forget` | Account pairing (also from the orb or Settings) |
| `KBX talk` | Same as a tap on the orb |
| `KBX volume <0-100>`, `KBX lang de\|en` | Persisted speaker volume and speech/UI language |
| `KBX status`, `KBX heap`, `KBX mic?`, `KBX loop?` | Diagnostics; `loop?` plays a soft 0.4 s tone and reports the mic level |
| `KBX shot` | Streams the screen; `tools/c6-screenshot.py COM7 out.png` saves it |
| `KBX inject <bytes> [wake]` | Test turn whose input is raw 16 kHz S16LE mono sent over the port instead of the microphone; with `wake` the clip goes to the wake engine first and its hit starts the turn |
| `KBX wake off\|kombify\|hey_kombify\|hey_kubi\|hey_jarvis`, `KBX wake?` | Wake word selection and engine statistics (hits, inference time, highest probability since the last query) |
| `KBX settings` | Open the settings sheet (for screenshots) |
| `KBX off` | Power off through the PMU |
| `KBX reboot` | Restart |

## Evidence (2026-10-22, unit `fc:01:2c:fd:de:a4`, source as committed)

- Boot: panel variant V1 detected, display, touch controller and ES8311 up;
  heap after UI ~179 KB.
- Pairing: device grant shown as QR and code on the AMOLED, approved as the
  shared fixture `TEST_PRO_USER`, `paired=1`; refresh after reboot works.
- Voice: cold turn after reboot minted a Gateway session (HTTP 201), WSS
  connected, `listening` after ~12.4 s (warm ~7.7 s); an injected German
  question produced `input_transcript`, a 245,760-byte 24 kHz reply played on
  the speaker, and the German caption rendered with umlauts.
- Three injected turns in a row each played a reply; free heap stayed at
  ~172 KB with a 65 KB minimum. An earlier heap-allocated inject buffer had
  fragmented the heap down to a 4 KB minimum and broke the next pairing; it
  is static now.
- Microphone: `KBX loop?` raised mic RMS from 752 to 3898 during the tone.
- Wake word: an injected "Hey Jarvis" plus German question hit (mean 0.97 to
  0.99), the turn listened after 2.7 s with 1.56 s of speech buffered and no
  frame dropped, the question was transcribed and a 211,200-byte reply played.
  A tap turn listened after 1.9 s. Session create 0.5 to 1.0 s and WSS dial
  0.9 s, both resumed.
- Not yet shown: a wake word and question spoken acoustically by a person, a
  physical tap on the orb, and a kombify wake phrase that triggers on a real
  voice; the first Gateway attempt of the day returned one transient HTTP 502.
