# microWakeWord models

The firmware embeds these models (`firmware/components/kbx_wake/CMakeLists.txt`) and runs them
with TensorFlow Lite Micro (`firmware/components/kbx_wake/wakeword_mww.cc`). Each `.tflite`
file has a `.json` manifest with its phrase, detection cutoff and tensor arena
size.

| Model | Phrase | Origin | License |
| --- | --- | --- | --- |
| `kombify.tflite` | "Kombify" | trained by kombify with [microWakeWord](https://github.com/kahrendt/microWakeWord) | [kombify Wake Word Model License](LICENSE-KOMBIFY-WAKE-WORDS.md) (source-available) |
| `hey_kombify.tflite` | "Hey Kombify" | trained by kombify with microWakeWord | [kombify Wake Word Model License](LICENSE-KOMBIFY-WAKE-WORDS.md) (source-available) |
| `hey_kubi.tflite` | "Hey Kubi" | trained by kombify with microWakeWord | [kombify Wake Word Model License](LICENSE-KOMBIFY-WAKE-WORDS.md) (source-available) |
| `hey_jarvis.tflite` (v2) | "Hey Jarvis" | Kevin Ahrendt, [micro-wake-word-models](https://github.com/esphome/micro-wake-word-models) `models/v2/hey_jarvis.tflite` (commit `40ff33f`) | Apache-2.0 |
| `stop.tflite` (v2) | "Stop" | Kevin Ahrendt, [microWakeWord](https://github.com/kahrendt/microWakeWord) release `stop` | Apache-2.0 |

The three kombify models (with their `.json` manifests) are excluded from the
Apache License 2.0 of this repository. You may view them and redistribute them
unmodified with a copy of this repository; running them in your own firmware
build, product or service, or deriving models from them, needs kombify's prior
written permission. "Kombify", "Hey Kombify" and "Hey Kubi" are kombify
trademarks. Details: [LICENSE-KOMBIFY-WAKE-WORDS.md](LICENSE-KOMBIFY-WAKE-WORDS.md).

The kombify models were trained on synthetic and public speech data; those
sources and their licenses are listed in
[THIRD-PARTY-NOTICES.md](../../../THIRD-PARTY-NOTICES.md). No recording from
those sources is distributed here. "Hey Jarvis", "Stop" and the ESP-SR WakeNet
models keep their upstream licenses.
