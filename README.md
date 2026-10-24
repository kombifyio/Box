# kombify Box

Open-source firmware for the kombify Box voice satellites: small ESP32 boards
with a microphone, a speaker and a display that wake on a wake word or a tap and
stream one bounded voice conversation to a kombify voice server, either kombify
Cloud or your own [SpeechKit](https://github.com/kombifyio/SpeechKit) server.
Speech recognition and the agent run on the server; the device runs the wake
word and the stop word on board and holds no provider keys.

## Boards

| Board | Source | Release assets |
| --- | --- | --- |
| Waveshare ESP32-S3-Touch-LCD-1.85C (round 360x360, touch) | [`firmware/`](firmware/README.md) | `box-firmware.json`, `kombify_box-<source>.bin` |
| Waveshare ESP32-S3-RLCD-4.2 (reflective 400x300, buttons) | [`firmware/`](firmware/README.md), `KBX_BOARD` RLCD 4.2 | `box-firmware-waveshare-esp32s3-rlcd-4.2.json`, `kombify_box-waveshare-esp32s3-rlcd-4.2-<source>.bin` |
| Waveshare ESP32-C6-Touch-AMOLED-1.8 | [`firmware-c6-amoled/`](firmware-c6-amoled/README.md) | not released yet (USB flash only) |

## Releases and updates over Wi-Fi

Every release on this repository's
[Releases page](https://github.com/kombifyio/Box/releases) carries one image
and one update manifest per released board. A Box checks
`https://github.com/kombifyio/Box/releases/latest/download/box-firmware<board>.json`,
verifies the manifest's board, size and SHA-256 and installs the image only
when its ELF digest differs from the running one. The manifest is the only
image authority: the device accepts images only from this repository's
releases.

## Build

Install ESP-IDF 6.0 (the release images use `espressif/idf:v6.0.3`), open an
ESP-IDF shell and build from `firmware/`:

```bash
cd firmware
idf.py set-target esp32s3
# Waveshare ESP32-S3-Touch-LCD-1.85C
idf.py build
# Waveshare ESP32-S3-RLCD-4.2
idf.py -B build-rlcd42 -D SDKCONFIG=sdkconfig.rlcd42 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.rlcd42" build
# flash over USB
idf.py -p <port> flash
```

A release image is built with `-DPROJECT_VER=<version>` added to `idf.py`; the
update manifest checks that version. The first build loads the pinned
components (`firmware/main/idf_component.yml`, `firmware/dependencies.lock`)
from the ESP Component Registry. `firmware/Dockerfile.ci` builds both boards and
runs the host boundary tests in the pinned ESP-IDF container. The ESP32-C6
AMOLED variant is not released yet; see
[`firmware-c6-amoled/README.md`](firmware-c6-amoled/README.md). More detail:
[`firmware/README.md`](firmware/README.md).

This repository runs no CI of its own. Before publication the release pipeline
builds both released boards from this exported tree with `firmware/Dockerfile.ci`;
a build failure prevents the source push and release. The images and manifests
attached to the release come from that public-source build.

## Wake word for self-built images

Self-built images must use a third-party wake phrase: "Hi ESP" (the ESP32-S3
default), "Jarvis", "Computer", "Mycroft" or "Hey Jarvis" (`KBX wake phrase
<id>` over USB, or the device settings). The kombify phrases "Kombify",
"Hey Kombify" and "Hey Kubi" are reserved for official kombify firmware
releases: running their models in any other build needs kombify's prior
written permission
([kombify Wake Word Model License](firmware/models/mww/LICENSE-KOMBIFY-WAKE-WORDS.md)).

## Documentation

- [USB and LAN console commands](docs/cdc-protocol.md)
- [Audio capture contract](docs/audio-capture-contract.md)
- [Device baseline and on-device constraints](docs/device-baseline.md)
- [Wake-word models and their licenses](firmware/models/mww/README.md)

## Contributing

This repository is generated from the maintainers' source tree for every
release; pull requests are reviewed and ported there. Please open an issue
first for larger changes.

## Security

Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md).

## License

Apache License 2.0, see [LICENSE](LICENSE), with one exception stated in
[NOTICE](NOTICE): the kombify wake word models
`firmware/models/mww/{kombify,hey_kombify,hey_kubi}.{tflite,json}` are not
under Apache-2.0. They are source-available under the
[kombify Wake Word Model License](firmware/models/mww/LICENSE-KOMBIFY-WAKE-WORDS.md):
view and redistribute them unmodified with this repository; any other use
needs kombify's permission. "kombify", "Hey Kombify" and "Hey Kubi" are
kombify trademarks. Third-party components and models, among them "Hey
Jarvis", "Stop" and the ESP-SR WakeNet models, keep their upstream licenses,
listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
