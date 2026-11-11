# CDC protocol (box ↔ host)

The box TinyUSB CDC-ACM port is both a log mirror (box→host) and a command
channel (host→box). Line-based: `\n` terminates, `\r` is ignored, inbound
lines max 2047 characters (`usb_log.c CDC_RX_LINE_MAX 2048`; raised for JWT
bearers during `net_ws` provisioning; oversize lines are discarded until the
next `\n` and logged).

**Source of truth:** firmware `firmware/main/usb_log.c` (parser) +
`firmware/main/box_status.c` (state vocabulary); companion side
the SpeechKit USB companion host (`boxlink.go`). The contract is
pinned by `boxlink_test.go` (wire framing + log filter) — changes here require
changes there.

## Host → box

**Commands** (tone/testtone/touch?/wifi…) including the `KBX ` prefix are fully
case-insensitive; the prefix is optional (`usb_log_line_command_is`). For
**state lines** only the state word is case-insensitive; the prefix is stripped
case-sensitively — exactly `KBX `, `kbx `, or `state=` (`KbX wake` and
`STATE=idle` are dropped silently, `box_status.c:64-68`).
Companion wire format: `KBX <word>\n`.

| Command | Effect |
|---|---|
| `KBX tone` / `KBX testtone` | Local test tone on the box speaker |
| `KBX touch?` | Touch diagnostics: logs `kbx_touch: status: init=… task=… awake_cfg=… polls=… presses=… actions=… read_errs=… last=(x,y)` (`awake_cfg=1` = DisAutoSleep 0xFE active, chip never sleeps → `read_errs` stays constant; with `awake_cfg=0` high `read_errs` are normal until a tap) |
| `KBX mic?` | Mic diagnostics: `kbx_audio: status: vu=… lpeak=… rpeak=… seq=… uac_gaps=… host_mute=… local_mute=… es7210=…` |
| `KBX mic probe` | Raw per-slot level of the ES7210 for 3 s, before the cue gate and any gain: `kbx_audio: mic probe n/3: left rms=… peak=… dBFS \| right rms=… peak=… dBFS \| speaker blocks=… gated=…`, then a verdict line naming the louder slot and the slot in use. Speak during the probe; the microphone slot is the one that follows your voice |
| `KBX mic slot left\|right\|mix\|auto`, `KBX mic slot?` | Microphone slot for the uplink and the wake word (persisted). `auto` uses the slot learned this boot (`microphone slot learned: …`), else the mix of both slots. Applies from the next turn or wake-word reset |
| `KBX voice auto\|female\|male`, `KBX voice?` | Reply voice gender (`assist.voice`). `auto` is male for the wake phrases `jarvis`, `hey_jarvis`, `mycroft` and `computer`, female otherwise. de and en send `aura-2-viktoria-de`/`aura-2-julius-de` and `aura-2-thalia-en`/`aura-2-apollo-en`; each start logs `net_ws: start: locale=… voice=…` |
| `KBX wake gain <1-16\|0>` | microWakeWord input gain (default x1, persisted; ESP-SR keeps the uplink's x14); restarts a running microWakeWord engine |
| `KBX wake?` | Wake word status: `kbx_wake: wake: … detect avg=<us> max=<us> … resets=<n> (lost=<n> last=<reason>@<uptime s>) gap max=<ms> first=<ms> …`. `lost` counts context losses (`mute`, `overrun` = the detector read the capture ring more than 480 ms late, `overwritten`, `capture-gap`, `dsp`); a busy ring lock is retried, not lost. `gap max` is the longest wait between two frames while listening (stays far below 480 ms when nothing starves the detector), `first` the first inference after an engine build (lazy model setup, kept out of `max`) |
| `KBX cloud pair` | Start the Box's own Cloud device grant and show the phone approval QR/code. No PC runtime is required after approval. |
| `KBX cloud enroll <ticket>` | USB link from a signed-in kombify client (SpeechKit desktop "Set up device"), in place of the device grant. The ticket (`kee_` + 43 base64url characters, single use, 10 minutes) is redeemed at `POST https://api.kombify.io/v1/connect/edge-enrollment/redeem`; the Gateway exchanges it at Auth0 for this Box's own refresh credential under the Box client. The Box derives its device id from the returned subject and its factory MAC and drops the credential unless it equals the id the ticket was bound to. Logs `cloud_auth: enroll: started`, then `cloud_auth: enroll: linked device=dev_…` or `cloud_auth: enroll: failed reason=<code> http=<status>`; the ticket is never logged. USB only (the LAN console refuses it); refused while the image does not match the board. Same command on the C6 console. |
| `KBX cloud?` | Pairing/paired state, remaining lifetime, approval URL and public user code; never an access, refresh or device code. |
| `KBX cloud forget` | Immediately stop local media and remove the persisted Cloud refresh credential. A fresh physical pairing is required to reconnect. |
| `KBX ui?` | UI diagnostics: `kbx_display: status: chrome=… vu=… dt_ms=… max_dt_ms=… stalls=… anims=… paused=… aura=…`. Stall: `error_code=ui_timer_stall` (tag `kbx_display`). |
| `KBX wifi <ssid>\|<pass>` | Wi-Fi credentials into NVS (pipe separates SSID/pass; both may contain spaces) |
| `KBX wifi?` | Log Wi-Fi status (`net_wifi: status: …`) |
| `KBX wifi clear` | Delete credentials |
| `KBX server <url>` / `KBX server?` | speechkit-server base URL into NVS / log status (`net_ws.c`) |
| `KBX server auto` | Find a homelab speechkit-server on the LAN (`lan_pair.c`): DNS-SD browse of `_speechkit._tcp`, the instance whose TXT `modes` include `voiceagent` (one announcing `pair=1` wins), its `url` into `net`/`sk_url`. A different server clears the old URL and token first. Then, if no token is stored, LAN device pairing as `KBX lan pair`. Logs `lan_pair: discover: <instance> url=<url> pair=<0\|1>`; a TXT with a credential key is ignored. Nothing found: `lan_pair: pairing failed: no_server` and the display says so |
| `KBX lan pair` | LAN device pairing (SpeechKit `speechkit.pairing.v2`) with the stored LAN server: `POST /v1/pairing/device-requests`, the server's 6-digit code on the display (`lan_pair: code <code> on <url> - approve on the server: speechkit-server pairing approve <code>`), polling until approved, denied or expired (10 min). The approved per-device token (`skd_…`) is stored as the `net`/`sk_token` bearer (never logged), `agent on` is set, `lan_pair: paired with <url>; device token stored`. The shared server bearer is never needed. Failure reasons: `offline`, `no_server`, `unreachable`, `pairing_off`, `not_lan`, `busy`, `denied`, `expired`, `server_error`, `invalid_response`, `store_failed`, `server_changed` (the server was changed or cleared, or Cloud pairing began, while the code was pending; no token is stored) |
| `KBX lan cancel` / `KBX lan?` | Stop a running discovery or pairing / `lan_pair: status: phase=idle\|browsing\|requesting\|show_code\|paired\|failed server=… instance=… code=… expires=… error=…` (never a secret) |
| `KBX token <bearer>` / `KBX token?` | Service bearer into NVS (never logged) / log length only |
| `KBX agent on\|off\|?` | Arm/disarm/status of touch-to-talk; arming does not open a provider session |
| `KBX provider <name>\|clear\|?` | Voice-agent provider for the start frame (empty/clear = server default, recommended) |
| `KBX net?` / `KBX net clear` | Combined `net_ws` status / clear provisioning |
| `KBX config?` | Account device settings (`speechkit.box_settings.v1`, `device_config.h`): `kbx_cfg: config: … applied=<rev> local_pending=<0\|1>`, the local desired values as JSON and the reported firmware, wake state and offered phrase ids |
| `KBX config apply <json>` | Apply a desired-settings document exactly as the account sync does, e.g. `KBX config apply {"schema":"speechkit.box_settings.v1","revision":3,"wake":{"enabled":true,"phrase":"jarvis"}}`. Logs `kbx_cfg: config apply: applied\|current\|stale\|rejected <reason>`. Newer revision wins over a pending local change; an invalid field rejects the whole document. `KBX wake on\|off\|phrase` and the Quick Settings wake controls mark a local change for write-back |
| `KBX frontdoor route <utterance>` / `KBX frontdoor mode oneshot\|conversation` | Test console for the server's front-door receipts (`route_switch`, `conversation_mode`): builds the receipt for the running cloud session and feeds it through the socket's own handler (`net_ws_debug_receipt`), so the Companion hand-over and the mode switch can be proven before the SpeechKit release. Does nothing without an active cloud session |
| `KBX talk` / `KBX talk?` | Toggle the talk phase (same as a tap on the orb) / talk status. Mute is a hold on the display, not this command. |
| `KBX version?` | Running slot, app version, ELF SHA, OTA image state and the image's board marker (`kbx_ota: version: … image_board=<id>`), then the board identity (`kbx_board: board: compiled=<id> probed=<id\|unknown> source=i2c\|nvs\|none mismatch=0\|1 (<probe detail>)`), `kbx_board: identity: edge=<dev_…\|-> usb_serial=<KBX-…>` and the `KBX hw?` line |
| `KBX hw?` | The one line host onboarding reads: `kbx_hw: hw: chip=esp32s3 mac=<factory MAC> board=<probed id\|unknown> image=<compiled id> match=yes\|no\|unknown via=i2c\|nvs\|none`. `board` is the hardware (early-boot I2C probe: the 1.85C answers 0x20 TCA9554 or 0x15 CST816S plus one more on SDA11/SCL10, the RLCD 4.2 0x70 SHTC3 plus one more on SDA13/SCL14; a conclusive probe is cached in NVS), `image` the board the running image was built for |
| `KBX board?` | `kbx_board: board: …` line plus the `KBX hw?` line |
| `KBX reboot` | Restart the current image with stored settings intact; refused during an active firmware update. |
| `KBX cloud show` | Display account connection feedback without starting a new login. |
| `KBX sync?` / `KBX sync now` / `KBX sync body` | Edge device sync (`edge_sync.h`, kombify Connect contract section 9.2): status (`kbx_sync: sync: base=… id=dev_… block=… http=… heap=<free>/<largest> need=<free>/<block> cost=<B> low_heap=<s> yields=<n>`, seen/applied revision; a cycle needs the wake word's 10 KB keep floor plus what the last cycle measured (`cost`, logged as `sync: cycle done, internal RAM <before> -> low <min> (cost <B>)`, 4 KB until measured) plus 1 KB free, and one 2 KB block; `block=low-heap` means that is not free, and after one poll period of it the warm voice session yields (`block=warm-yield`, `net_ws: warm: held for the settings sync`) until the cycle ran; an account revision below the applied one that stays 180 s is adopted (a pending local change is written on it, otherwise its document applies), `poll=<age>s/<period>s` where the period is the Gateway's `x-kombify-poll-after` hint clamped to 5..300 s and 60 s without one, firmware policy, handled update request), check-in and poll as soon as the box is quiet, and the check-in and write-back bodies the box would send now |
| `KBX sync put [v<rev>] <settings json>` | Development bench builds only (`firmware/sdkconfig.defaults.dev`). Bench writer: one `PUT .../settings` with `If-Match "v<rev>"` (default the last seen revision) by the box's own identity, as an outside writer would send it; the box applies the new revision on its next poll. Logs `kbx_sync: sync: bench put … http=<status>` |
| `KBX sync base [?]` | Shows the Gateway base of the sync: fixed to `https://api.kombify.io`. Any argument other than `?` is refused |
| `KBX update check` / `KBX update?` | Check the fixed public Box firmware manifest (`kombifyio/Box` releases) of the **probed** board over WLAN / report safe status. |
| `KBX update install` | Install only the firmware accepted by the last successful manifest check; on a box running another board's image this installs the image for its hardware. |
| `KBX ota?` | OTA job status (`phase`, `busy`, heap) |
| `KBX ota <url>` | Install only the exact HTTPS asset selected by the last accepted firmware manifest; arbitrary URLs are rejected. Box reboots on success. |
| `KBX ota-begin <bytes> [sha256]` | CDC OTA: **wait** for the log line `kbx_ota: recv`, then send exactly `<bytes>` of raw `kombify_box.bin`. Bytes before that line overflow the parser and wedge the COM port. Optional 64-char SHA-256 is verified before the boot slot switches; verification allocates internal RAM (`esp-sha`) and fails when heap_int is ~2–3 KB (typical with Wi-Fi + session). |
| (board check) | Every OTA path (`ota-begin`, LAN `/kbx/ota`, `update install`) reads the board marker in the first 352 bytes of the image (right after `esp_app_desc_t`, offset 288) before it opens the inactive slot. Another board's image is refused: `kbx_ota: refused: board_mismatch image=<id> box=<id> …`, then `kbx_ota: err board-mismatch` (CDC reads the remaining bytes and drops them). A raw push without a marker (built before it existed) is refused as `image-board-unknown`; a manifest update accepts it because the manifest names the board |
| `KBX bootloader` | Software reboot into the ROM serial download loader (USB PID `1001`) without holding BOOT and without USB persist. PID `0009` (USB-Serial/JTAG) is not the loader. If the box sticks in download mode after a watchdog, clear RTC `0x60008128`. |
| `KBX remote key <64 hex>` | Store the LAN console key (32 bytes) and start the console on port 8266. USB only; the key is never echoed or logged. |
| `KBX remote off` / `KBX remote?` | Erase the key and stop the console / report state and request counters. |
| `KBX aec on` / `off` / `type sr\|vc` / `?` | Echo cancellation against the codec loopback (RLCD 4.2 default on, `sr`); `?` shows mic, loopback and cleaned level of the last second. |
| `KBX stop?` / `KBX duck <dB>` | Spoken "Stop" while a reply plays: hits and frames / ducking margin over the echo residue (0 = off). |
| `KBX tdm?` / `KBX es7210 <reg> [value]` | Per-slot level of the TDM capture / read or write one ES7210 register (bring-up). |
| `KBX bat?` / `KBX env?` / `KBX env offset <tenths>` | Battery voltage and level / room climate, temperature correction (humidity follows it). |
| `KBX codec vol <0-100>` | ES8311 DAC volume until reboot (75 = 0 dB). |
| `KBX <state>` | Set product UI state (anything that is not a command goes to `box_status_set_from_text`; also drives the Aura Orb — useful for cycling UI states) |

**board_mismatch mode.** When the probe names another board than the image
was built for, the image never confirms itself: an unconfirmed OTA slot rolls
back to the previous one at once (`kbx_ota: board_mismatch: … rolling back`;
the next image shows `kbx_board: board: last rejected image <version> for
<id>`). Without a slot to return to (USB flash) the box runs without any board
driver (no display, touch, audio pins, sensors, wake word or voice) and logs
`board_mismatch` every 60 s. Only `version?`, `hw?`, `board?`, `reboot`,
`bootloader`, `update …`, `ota …`, `ota-begin`, `wifi …`, `sync …`, `cloud?`,
`coredump …`, `heap?` and `remote …` are served (CDC and LAN console); every
other `KBX` command logs `refused in board_mismatch`. The edge sync check-in
reports the hardware as `board` and `reported.board_identity` with
`mismatch:true`, and applies no account settings.

**State vocabulary** (`box_status.c`, prefix match on the first word,
optional `KBX `- or `state=`-prefix):

| Input (prefix) | UI state |
|---|---|
| `idle`, `ready` | IDLE |
| `wake` | WAKE |
| `listen…`, `capture…` | LISTENING |
| `think…`, `process…` | THINKING |
| `speak…`, `tts…` | SPEAKING |
| `done` | DONE |
| `error`, `fail…` | ERROR |

Unknown words are dropped (no error echo).

## Box → host

The full ESP_LOG output is mirrored — **only while DTR is set**
(`tud_cdc_connected()`, `usb_log.c`). Hosts must open the port with DTR=true
(`boxlink.go openBoxPort`, `scripts/kbx-cmd.ps1 DtrEnable`), or the mirror
stays silent.

Relevant log tags (companion filter `boxLogRelevant` mirrors only these plus
`E (`/`W (` lines as `[box] …`):

| Tag | Module |
|---|---|
| `net_wifi` | Wi-Fi status/provisioning (`net_wifi.c`) |
| `kbx_touch` | Touch events + `touch?` diagnostics (`touch.c`) |
| `oobe` | reserved in the companion filter — no firmware module (as of c90aa85) |
| `net_ws` | Standalone voice-agent client: session, talk uplink, downlink (`net_ws.c`) |
| `lan_pair` | Homelab discovery over DNS-SD and LAN device pairing (`lan_pair.c`) |
| `kbx_aura` | Aura Orb UI (`aura_orb.c`) |
| `kbx_display` | UI chrome, `ui?`, `error_code=ui_timer_stall` |
| `kbx_ota` | OTA apply, version, health/rollback (`fw_ota.c`) |
| `net_agent` | planned: B5.5 health heartbeat |

## Tools

- Companion: keeps the port open, sends stages automatically
  (`boxlink.go`), queries `KBX touch?` once on connect.
- Standalone (companion stopped — COM ports are exclusive):
  `powershell -ExecutionPolicy Bypass -File scripts\kbx-cmd.ps1 -Command "touch?"`
- App firmware OTA without download mode:
  `powershell -ExecutionPolicy Bypass -File scripts\ota-push.ps1`
  After the first image that contains `fw_ota.c` is on the device, later app
  updates use this path. USB download-mode flash (`KBX bootloader`, PID `1001`)
  remains recovery: restore of a known-good slot, partition-table changes, or
  CDC-OTA SHA starvation. Current device image: [device-baseline.md](device-baseline.md).
- Over Wi-Fi (LAN console, `firmware/components/kbx_core/lan_console.c`), once paired on USB:
  `scripts\kbx-remote.ps1 -Name <box> -Pair`, then `-Address <ip> -Command "<cmd>"`
  (reply = device log) or `-Ota <kombify_box.bin>` (SHA-256 checked on the box
  before the slot switch, Wi-Fi stays up). Every request needs a single-use
  nonce and an HMAC-SHA256 with the key; the transport is plain HTTP, so
  `wifi <ssid>|<pass>`, `token`, `remote`, `bootloader` and `ota-begin` are
  refused over the LAN and stay on USB.
