# Device baseline

## Standalone Cloud image on the reference device (verified 2026-10-18)

The reference Waveshare ESP32-S3-Touch-LCD-1.85 runs the public
[SpeechKit 0.77.6 firmware](https://github.com/kombifyio/SpeechKit/releases/tag/v0.77.6),
built from Box source `41aad7ee49cc47303183349ea00c2055f1765b65` with the pinned
ESP-IDF 6.0.3 toolchain and installed over WLAN: version `0.77.6`, 1,677,984
bytes, image SHA-256 `a7a4565ae4b70a5e63b8bf643393197972a8bbd3c9a8f35cd5f0a79520132519`,
full ELF SHA-256 `67d97e3857c17d5a0be95395a7f13a9d31cf7322ae03c686b4b88926034bc7ac`,
valid OTA0 at `0x20000`. Cloud login is the Box's own Auth0 device grant,
followed by Gateway session creation and ticket-only WSS.

Observed on the device:

- Phone approval once; after a reboot the stored refresh credential renews
  without another approval and a fresh Gateway WSS session connects.
- Direct WLAN Talk: 619 microphone frames sent with zero faults, 241,920 bytes of
  24 kHz response PCM played, natural teardown, no reopened session through the
  51-second checkpoint, maximum UI interval 235 ms, zero stalls.
- Armed (WLAN and agent on) without an explicit Talk, no provider session is
  open; after 21 minutes uptime no session had reopened. A silent request ends
  15 seconds after the provider reports listening, and an update check during
  startup cancels capture and deletes the pending ticket.
- WLAN update: the installed firmware fetched the public manifest, downloaded and
  validated the complete image against it, rebooted after about 45 seconds into
  the inactive slot and kept WLAN, the paired account and agent-on; a later check
  reported CURRENT.
- Touch acceptance: a physical orb tap starts Talk without CDC activation,
  the answer is audible and the screen stays usable. This version handles one
  question and answer per activation (conversation mode is planned).
- Native boundary tests for cJSON credential parsing, Gateway URL construction,
  FIFO ordering and scrub, 64 KiB protocol reassembly and OTA manifest authority
  passed in the affected gate.

Constraints found on the way, still binding:

- A valid Auth0 access token was 4,758 bytes; the credential and header buffers
  share an 8,192-byte bound in PSRAM.
- The output default is 24 kHz to match the provider; a 16 kHz default stretches
  playback.
- Explicit playback completion is signalled so a reply below the 32,000-byte
  jitter prebuffer starts and drains; the exact short-reply boundary is
  unverified on the physical speaker.
- GitHub's signed asset redirect used an 875-character request target, so the OTA
  client's transmit buffer is set to 4096 bytes (ESP-IDF default 512).
- Only explicit Talk opens provider media; the earlier lifecycle opened billable
  sessions while idle.

No wake-word, far-field, physical-extraction resistance, hardware secure-boot or
signed-firmware claim follows from these checks.

## Failed on-device wakeword attempt: constraints for any retry

The first Jarvis/microWakeWord build crashed the product UI.

1. No TFLM `AllocateTensors`/frontend setup on `app_main` (`CONFIG_ESP_MAIN_TASK_STACK_SIZE` 3584 bytes; the panic reboots).
2. No interpreter on the LVGL/QSPI core (core 1): it starves the panel and looks like a UI crash.
3. Keep the four LVGL rotation animations off; they hang QSPI after the first loud spike (`aura_orb_set_paused(true)` required).
4. Internal RAM is scarce (idle ~12 KB, ~2-5 KB with a WebSocket session): rings, WS task stacks and any arena belong in the 8 MB octal PSRAM, not DMA/SHA/Wi-Fi blocks.
5. esp-tflite-micro 1.3.3 dropped `experimental/microfrontend`; `hey_jarvis` v2 was trained against it, so vendor it again or train a new model.
6. `hey_jarvis` v2 is the phrase "hey jarvis", not the one-word product phrase; the Apache-2.0 model is a detector spike only.
7. CDC OTA SHA needs internal heap: pause voice and consider releasing Wi-Fi RAM before hashing. Sending image bytes before the `kbx_ota: recv` log line overflows the CDC parser and wedges the COM port.
8. `KBX bootloader` must not set USB persist, so ROM download re-enumerates as PID `1001`. `RTC_CNTL_FORCE_DOWNLOAD_BOOT` (`0x60008128`) survives a watchdog while the RTC battery holds; clear it if the box sticks in download mode.
9. Never rely on holding BOOT: use `KBX bootloader` or the open PID `1001` port. USB-Serial/JTAG (PID `0009`) is not the ROM loader.

Any retry starts from the current image without TFLM, then adds wakeword as its
own task on a PSRAM stack, off the UI core, with lazy initialisation outside
`app_main`.

Operator commands, OTA and recovery: [cdc-protocol.md](cdc-protocol.md), [firmware/README.md](../firmware/README.md).
