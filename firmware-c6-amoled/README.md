# kombify box C6 AMOLED

Cloud voice satellite firmware for the Waveshare **ESP32-C6-Touch-AMOLED-1.8**:
the third kombify box device beside the round S3
Box and the RLCD 4.2 board (both `firmware/`). Same standalone
Cloud profile as the S3 Box: account pairing by phone QR, tap or wake word to
talk, one bounded voice session per tap with follow-up turns while the reply
expects an answer, remote management through the Gateway edge-device
directory, no provider keys and no PC at runtime.

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
| Conversation mode | yes | yes (same `expects_reply` rule, half-duplex) |
| Edge-device directory and account settings | `edge_sync.c` (PSRAM task, own HTTP client) | `c6_edge.c` (rides on the voice TLS transport, idle only) |
| WLAN updates, sound cues, voice and route settings | yes | not yet |

Shared without copies: `cloud_auth.c`, `net_wifi.c`, `session_url.c`,
`voice_protocol.h`, `voice_error.h`, `ui_theme.h`, the kombify rosette and the
`kbx_lat` fonts and the microWakeWord engine (`wakeword_mww.cc`) and models
from `firmware/components` and `firmware/models/mww`. The shared changes keep the S3
build identical: `cloud_auth.c` and `wakeword_mww.cc` pick internal RAM (and
smaller auth work buffers) only when `CONFIG_SPIRAM` is off,
`voice_protocol.h` lets a board pass a smaller `KBX_VOICE_TEXT_LIMIT`, and
`cloud_auth.c` takes the renewal margin from `CLOUD_AUTH_RENEW_MARGIN_S`
(default 600 s; the C6 uses 90 s, its token only mints a session).

## Conversation mode

After a reply whose SpeechKit `turn_end` carries `expects_reply: true` (or,
when the server sends no flag, whose last sentence ends in a question mark)
the box listens again on the same session and socket, without a tap, up to
`assist.max_followups` turns (0 to 3, default 3, persisted; Settings,
`KBX followups <0-3>` or the account document). This is the S3 rule
(`firmware/components/kbx_voice/net_ws.c`, `followup_wanted_locked`).

Half-duplex shape of a turn: listen, uplink, think, play the reply, close the
speaker, wait 350 ms so the speaker tail is not heard, reopen the microphone,
reset the capture FIFO, listen again. A follow-up stage ends quietly after 6 s
without speech (no error pill); a tap ends the utterance or cancels as in the
first turn; `no_reply` or the server closing the session ends the conversation.
The reply buffer, the TLS transport and the socket stay up across the turns.

## Front door (Voice Agent, Companion)

The quick session opts into the SpeechKit front door with the top-level start
field `front_door {version 1, routes ["agent"]}` (an older server ignores it).
The server then decides from each final transcript and answers with typed,
session-bound receipts:

- `conversation_mode` (mode `conversation`): the box listens again after every
  reply without a wake word, no stage limit, 15 s of silence ends it; mode
  `oneshot` ends the conversation and the usual follow-up rule applies again.
- `route_switch` (`to: agent`, `utterance`): the box stops the quick session,
  opens the Companion route for this turn only (history, then a registered
  `kombify-agent` session; the saved settings are untouched), sends the
  utterance as the first `text` frame, shows "Frage den Companion …" and plays
  the answer. The next wake word or tap starts on the quick route again. The
  ring buffer is released before the Companion session is minted, as at a turn
  start.

An account that already runs its maximum of conversations gets the localized
`per_user_limit_exceeded` sentence on the screen (the pill says "Anderes
Gespräch läuft noch").

## Screen

- **Quick settings:** a downward swipe that starts in the top 64 px opens the
  settings (56 px of travel, mostly vertical), as the round S3 board does from
  its top bezel (`firmware/components/kbx_ui/touch.c`). The corner button
  stays, now 56 px; a grab handle at the top edge shows where to swipe. Every
  button in the sheet is 56 px high and the volume slider has a 20 px larger
  touch area. LVGL reports touch-down and release to the input device, the
  points in between are polled every 25 ms (`c6_ui.c`, `swipe_check`).
- **Idle and active:** the values are the shared ones in `ui_theme.h`
  (`KBX_IDLE_MARK_*`, `KBX_STATE_FADE_MS`). At rest the kombify mark keeps 45 %
  of its colour (recoloured toward grey) at 62 % brightness; wake, connecting,
  listening, thinking, speaking and error take full colour over 260 ms. The S3
  bakes a grey copy of the mark in PSRAM; without PSRAM the C6 recolours the
  one mark at run time, with the same percentages.
- **Two designs:** `watch` (default) and `classic`, selected per device by the
  account (`display.design`, `KBX design classic|watch`; the box restarts into a
  changed design when idle). Watch is the Pixel Watch layout: the orb is the
  face (320 px between two 64 px rims, the mark 186 px, a glow under the rims),
  the top rim carries Wi-Fi, battery (AXP2101 gauge, only with a battery), the
  time (CET/CEST computed in `c6_ui.c`, no libc timezone state) and the 56 px
  settings button, the bottom rim the state pill; the whole face is the talk
  target. The glow is a 96 px A8 map in flash (`kbx_orb_glow.c`), the mark a
  pre-baked 186 px asset, the reply overlay a ground scrim instead of a group
  opacity, so the larger orb costs no RAM.
- **Reply overlay:** while the user's recognised sentence or the assistant's
  current sentence is shown, the orb sits at 20 % opacity (the S3's 80 % ground
  scrim, `KBX_REPLY_SCRIM_PCT`) and that one sentence (the last sentence of the
  transcript, never a log) is drawn in front in the 28 px face, the 20 px face
  above 110 characters. It stays 4 s after the turn, then the orb returns.

## Edge-device management

`c6_edge.c` implements the device side of the Connect contract section 9.2 for
the C6, the same routes, device id derivation and document
(`speechkit.box_settings.v1`) as the S3 boards
(`firmware/components/kbx_core/edge_sync.c`, `device_config.c`):

- The device id is `dev_` plus the base64url HMAC-SHA256 of
  `kombify-connect-device:v1:<sub>` keyed with the factory base MAC, `sub` read
  from the box's own access token. Board `waveshare-esp32c6-touch-amoled-1.8`,
  kind `kombify-box`, display name `kombify Box C6`.
- A cycle is a check-in (boot, a changed reported value, every 5 min) or a
  settings poll with `If-None-Match`, and a write-back with `If-Match` for a
  local change (volume, wake word, follow-ups, language; it settles 4 s so a
  slider drag is one write). An account without a document is seeded from the
  box. A newer account revision replaces an unsent local change. The poll
  period is the Gateway's `x-kombify-poll-after` (clamped to 5 to 300 s, 60 s
  without it), so an open dashboard reaches the box in about 10 s.
- No second TLS stack: without PSRAM the cycle runs inside the voice task's idle
  upkeep on the voice transport (the TLS ticket resumes, a cycle takes 0.3 to
  0.5 s), while the wake engine is parked, and only when the box is idle. The
  working memory (request buffer, and the response buffer shared with the voice
  path, now 6 KB) is static.
- Applied: `wake.enabled`, `wake.phrase` (Kombify, Hey Kombify, Hey Kubi, Hey
  Jarvis as `kombify`, `hey_kombify`, `hey_kubi`, `hey_jarvis`),
  `assist.max_followups`, `audio.volume`, `locale` (`de`, `en`). Not on this
  board and ignored: sound cues, reply voice, route and the firmware member.
  An invalid value rejects the whole revision and is reported in
  `sync.rejected_revision` and `reject_reason`.
- An account revision below the applied one is first taken for a lagging read;
  when the account still serves it 180 s later (`stale=<rev>/<s>` in
  `KBX sync?`) the box adopts the account's numbering: a pending local change
  is written on that revision (If-Match), otherwise the account's document
  applies.
- `KBX sync?` shows the state, `KBX sync now` asks for a cycle at the next idle
  upkeep. A bench build can point the sync at another origin with
  `idf.py -DC6_EDGE_BASE=https://gateway.preview.invalid build` (reset it
  with `-DC6_EDGE_BASE=`); it also stops the voice TLS warm-up, because one
  transport holds one ticket.

## Memory

| Point | Free heap |
| --- | --- |
| After Wi-Fi | 227 KB |
| After UI, voice, wake engine loaded | 103 KB |
| Idle (engine loaded) | 54 to 72 KB |
| Turn start (engine unloaded) | 105 to 111 KB |
| Stage start, WSS open, 24 KB reply ring up (minimum over a 3-stage conversation) | 45 KB (20 KB) |

The draw and DMA buffers are 20 lines (`LVGL_BUF_LINES` in `c6_ui.c`) instead of
32: with 32 lines the first uplink of a turn failed on an mbedTLS allocation.

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
- **Learning that the server verifies:** a wake turn that was asked for its pre-roll (`state: verifying`) marks the server as verifying, persisted (`c6_voice/verifies`). Only then does the engine use the candidate cutoff. A turn unlearns it only when the server answered `listening` without `verifying` and the turn ended without an error, never on a 503, limit, budget or auth error. Until 2026-10-25 the check read the socket flag after `ws_close()` had cleared it, so the candidate cutoff never engaged on a real board.
- **Tuning:** `KBX wake cutoff <0.3-0.999|default>` is the cutoff used alone, `KBX wake cand <...>` the one used while the server verifies (both persisted per board, values above 1.0 only until the next reboot, a damaged stored value is ignored; `KBX wake?` prints both and the one in use). The model's own values stay the default.
- **Timing:** the log carries `wake word verified|rejected by the server: N ms after the pre-roll request, M ms after the hit`.
- **Stage-one limits:** the kombify-trained Kombify and Hey Kubi models barely rise above their baseline on unseen voices. Stage one only helps them after retraining.

## microSD

Off by default, `KBX sd on` switches it on (persisted), `KBX sd off` off. Local
only: nothing on the card is uploaded or read back by the firmware.

- `/sdcard/kbx/wake/wNNNNN.wav`: the first 2 s sent to the server for each wake candidate (16 kHz mono, the pre-roll plus the start of the speech, after the 4:1 ADPCM FIFO).
- `/sdcard/kbx/wake.csv` (with a header line): `seq,uptime_s,epoch,phrase,score,verdict,verify_ms,file,dropped_frames`; the verdict is `accepted`, `rejected`, `local` (no server check), `failed`, `aborted` (capture switched off mid-clip), `card_error` (a card error closed the clip; the row is written after the next mount) or `lost` (the close never arrived). `dropped_frames` counts uplink frames the card queue had no room for; a clip with a non-zero count is truncated.
- `/sdcard/kbx/log/log.0..3.txt`: the console log, 256 KB per file, newest in `log.0`.

The card is wired to GPIO 10/11/18/6, but the AMOLED's QSPI bus holds the C6's
only general-purpose SPI peripheral and Waveshare's board package refuses to
mount the card beside the display. So the card is driven in SPI mode by a
bit-banged driver (`c6_sd.c`) behind FatFs; a card that is not FAT-formatted is
never formatted. All card access runs in one priority-2 task; the voice path and
the log hook only post messages and drop them when the queue is full
(`KBX sd?` counts drops). Needs about 12 KB of heap while on.

## Power and screen

- **Power off:** the last row of the scrollable settings list, styled as a destructive row (alert colours) and never next to Done. It asks "Wirklich ausschalten?" / "Power off now?" first; "Ausschalten" then cuts the AXP2101 rails through software power-off (REG 10h bit 0), "Abbrechen" returns to the menu without side effects. This also works on USB power. A short press on the PWR key turns the box on again. `KBX off` does the same. The bottom row of the sheet keeps only Done.
- **Screen timeout:** while idle, the AMOLED turns dark after 2 minutes without touch, which also guards against burn-in. A touch or any voice turn lights it again, and the waking touch never starts a turn.
- **Renewal memory:** idle upkeep lends the static 52 KB capture FIFO to `cloud_auth` as its renewal work area (`cloud_auth_set_work_area`). A renewal then needs only the TLS handshake from the heap. It had failed once an open settings sheet and the wake engine had fragmented the heap.

## Build and flash

The pinned toolchain is the same image as the S3 CI build:

```bash
docker build --file firmware-c6-amoled/Dockerfile.ci --output type=local,dest=firmware-c6-amoled/build/dist .
python -m esptool --chip esp32c6 -p COM7 -b 921600 write-flash @firmware-c6-amoled/build/dist/flash_args
```

`flash_args` is relative to `build/dist`; run esptool from that directory.
On a Windows host with the pinned ESP-IDF (`C:\esp\esp-idf-v6.0.3`) a local
build on a 7.5 GB machine takes `IDF_PY_BUILD_JOBS=3`.
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
| `KBX inject <bytes> [wake]` | Test turn whose input is raw 16 kHz S16LE mono sent over the port instead of the microphone; with `wake` the clip goes to the wake engine first and its hit starts the turn. The console stays usable between clips (`KBX shot` during the turn) |
| `KBX inject more <bytes>` | The clip for the next follow-up turn of the running test; send it when the log says `follow-up stage` |
| `KBX wake off\|kombify\|hey_kombify\|hey_kubi\|hey_jarvis`, `KBX wake?` | Wake word selection and engine statistics (hits, inference time, highest probability since the last query) |
| `KBX wake cutoff\|cand <0.3-0.999\|default>` | Stage-one cutoffs: alone / while the server verifies |
| `KBX sd on\|off`, `KBX sd?`, `KBX sd ls` | Optional microSD capture of wake candidates and a rotating log (see "microSD") |
| `KBX settings` | Open the settings sheet (for screenshots) |
| `KBX swipe` | Run the top-edge swipe decision on a synthetic stroke and log the result (no touch needed) |
| `KBX followups <0-3>`, `KBX followups?` | Conversation mode limit (also an account setting) |
| `KBX frontdoor route <utterance>`, `KBX frontdoor mode oneshot\|conversation` | Test console: a front-door receipt for the open session (see Front door) |
| `KBX sync?`, `KBX sync now` | Edge-device sync state, check in at the next idle upkeep |
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

## Evidence (2026-10-25, unit `fc:01:2c:fd:de:a4`, build of this change)

- **Coredump found at boot (11 264 B, `coredump` partition):** `assert failed:
  xStreamBufferReceive stream_buffer.c:976 (pxStreamBuffer->xTaskWaitingToReceive
  == NULL)` in task `c6_capture`, with the `c6_wake` task alive beside it. The
  only `xStreamBufferReceive` in `c6_capture` reads the `KBX inject` test
  stream, which the wake task reads too after a `wake` inject: two readers on
  one stream. It is a test path (no microphone or tap turn reads that stream),
  fixed by handing the stream to one reader at a time (`c6_voice_inject_stream`)
  and by waiting up to 500 ms for the capture task to leave its loop
  (`capture_stop`). The ELF of that build was not kept, so the decode is the raw
  note data (panic reason, task list, registers) read with a small ELF-core
  parser, not a symbolised backtrace. The dump was erased from the board after
  it was read.
- **Conversation:** an injected German TTS question ("Erinnere mich bitte an
  etwas.") against the production Gateway: reply with `expects_reply`, then
  `follow-up stage 1/3 listening`, an injected answer, a second reply with
  `expects_reply`, `follow-up stage 2/3`, a third answer and a reply with
  `no_reply` that closes the session. One session, one socket, no tap; the
  replies played 336 000 B, 562 560 B and 171 836 B and the whole conversation
  took 60 s. Without a clip a follow-up ends after 6 s with "no speech" and no
  error pill. One of five runs lost the socket mid-follow-up (the server side
  closed the connection after a write timeout); the box ended the conversation
  without an error.
- **Screen:** `KBX shot` at idle (greyed mark), while a reply is shown (orb at
  20 %, the sentence in 28 px) and in the follow-up listening state (full
  colour, no sentence).
- **Edge-device management:** the first check-in returned 200 and seeded the
  account document at revision 1; the box is listed under the shared Pro
  fixture as `kombify Box C6` (`waveshare-esp32c6-touch-amoled-1.8`). A
  dashboard-equivalent PUT of `audio.volume` 47 (revision 2) was applied by the
  next poll (`applied revision 2`), the check-in then reported `applied=2`; a
  local `KBX volume 33` became a write-back (`account at revision 3`) 15 s
  later. Against the Preview Gateway with a dashboard open the box logged `poll
  period 10 s (Gateway hint 10 s)` and polled every 11 to 12 s; the production
  Gateway does not send the hint yet, so there the period stays 60 s.
- **Not shown:** the swipe on the real touch panel (`KBX swipe` runs the same
  decision on a synthetic stroke), a wake word and follow-up answers spoken by a
  person, and the 10 s poll against the production Gateway.

## Evidence (2026-10-25, wake word "kombify", unit `fc:01:2c:fd:de:a4`)

- **Stage one by itself** (`KBX wake cutoff 1.05`, highest probability per injected neural-TTS clip, 59 phrase clips of 18 voices, 48 other sentences): at the manifest cutoff 0.994, 39 of 59 phrase clips reach it and none of the sentences; at the candidate cutoff 0.5, 52 of 59 and 2 of 48 (the near-phrases "Hey Kubi, mach Musik" at 0.91 and "Ich fahre eine Kombi" at 0.68, which the server check removes); at 0.3, 55 of 59. The idle room peaks at about 0.18.
- **Production chain before this change** (9 voices through `KBX inject ... wake`): hit to the server's `verifying` 1.6 to 3.2 s; `verifying` to `wake_verified` 4.0 to 5.5 s; the server's own verification 1.09 to 1.58 s (Deepgram, 3 s window).
- **microSD:** `KBX sd on` without a card logs `no usable card (ESP_ERR_NOT_FOUND); retry in 60 s` and changes nothing else. Not shown: a mounted card, a captured clip, a rotated log (no card was in the slot).
- **Not shown:** the candidate cutoff engaging after a verified wake turn. The production voice service answered session-create with http 503 and then `402 speechkit_budget_exhausted` for the shared fixture for the rest of the session.
