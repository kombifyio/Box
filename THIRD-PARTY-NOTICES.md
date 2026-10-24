# Third-party notices

kombify-Box firmware bundles or links the components below. Versions are the
exact pins in `firmware/main/idf_component.yml` and `firmware/dependencies.lock`;
refresh this file whenever a pin changes. Registry components are restored at
build time into the gitignored `firmware/managed_components/` and are not
vendored in this repository.

| Component | Version | License | Source |
| --- | --- | --- | --- |
| ESP-IDF (core, FreeRTOS, mbedTLS, drivers) | 6.0.3 | Apache-2.0 (components carry their own SPDX headers) | https://github.com/espressif/esp-idf |
| LVGL (`lvgl/lvgl`) | 9.3.0 | MIT | https://github.com/lvgl/lvgl |
| `espressif/esp_lvgl_port` | 2.9.0 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_lvgl_port |
| `espressif/esp_lcd_st77916` | 2.0.2 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_lcd_st77916 |
| `espressif/esp_lcd_touch` | 1.2.1 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_lcd_touch |
| `espressif/esp_lcd_touch_cst816s` | 1.1.2 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_lcd_touch_cst816s |
| `espressif/esp_io_expander` | 1.2.1 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_io_expander |
| `espressif/esp_io_expander_tca9554` | 2.0.3 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_io_expander_tca9554 |
| `espressif/cmake_utilities` | 0.5.3 | Apache-2.0 | https://components.espressif.com/components/espressif/cmake_utilities |
| `espressif/tinyusb` (TinyUSB) | 0.19.0~3 | MIT | https://components.espressif.com/components/espressif/tinyusb |
| `espressif/esp_websocket_client` | 1.7.0 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_websocket_client |
| `espressif/cjson` (cJSON) | 1.7.19~2 | MIT | https://components.espressif.com/components/espressif/cjson |
| `espressif/esp-sr` (ESP-SR, WakeNet) | 2.6.0 | ESPRESSIF MIT (use on Espressif products) | https://components.espressif.com/components/espressif/esp-sr |
| `espressif/esp-dl` | 3.3.14 | MIT | https://components.espressif.com/components/espressif/esp-dl |
| `espressif/dl_fft` | 0.8.0 | MIT | https://components.espressif.com/components/espressif/dl_fft |
| `espressif/esp-tflite-micro` (TensorFlow Lite Micro; bundles FlatBuffers, gemmlowp and ruy, Apache-2.0, and KISS FFT, BSD-3-Clause) | 1.4.1 | Apache-2.0 | https://components.espressif.com/components/espressif/esp-tflite-micro |
| `espressif/esp-nn` | 1.4.1 | Apache-2.0 | https://components.espressif.com/components/espressif/esp-nn |
| `esphome/esp-micro-speech-features` (microWakeWord audio frontend, from the TensorFlow Lite Micro microfrontend; bundles KISS FFT, BSD-3-Clause) | 1.2.3 | Apache-2.0 | https://components.espressif.com/components/esphome/esp-micro-speech-features |

### ESP32-C6 AMOLED variant (`firmware-c6-amoled/`)

Pinned in `firmware-c6-amoled/main/idf_component.yml` and
`firmware-c6-amoled/dependencies.lock`. Besides ESP-IDF 6.0.3, LVGL 9.3.0,
`esp_lvgl_port` 2.9.0, `esp_lcd_touch` 1.2.1, `esp_lcd_touch_cst816s` 1.1.2,
`esp_io_expander` 1.2.1, `esp_io_expander_tca9554` 2.0.3, `cmake_utilities`
0.5.3, `esp_websocket_client` 1.7.0, `cjson` 1.7.19~2, `esp-tflite-micro` 1.4.1,
`esp-nn` 1.4.1, `esp-micro-speech-features` 1.2.3 and the four microWakeWord
models listed above and below, the
image links:

| Component | Version | License | Source |
| --- | --- | --- | --- |
| `waveshare/esp32_c6_touch_amoled_1_8` (board support) | 1.0.0 | Apache-2.0 | https://components.espressif.com/components/waveshare/esp32_c6_touch_amoled_1_8 |
| `waveshare/esp_lcd_sh8601` | 2.0.0 | Apache-2.0 | https://components.espressif.com/components/waveshare/esp_lcd_sh8601 |
| `espressif/esp_lcd_co5300` | 2.2.0 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_lcd_co5300 |
| `espressif/esp_lcd_touch_ft5x06` | 1.1.1 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_lcd_touch_ft5x06 |
| `espressif/esp_lcd_panel_io_additions` | 1.0.1~1 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_lcd_panel_io_additions |
| `espressif/esp_codec_dev` (ES8311 driver) | 1.5.11 | Apache-2.0 | https://components.espressif.com/components/espressif/esp_codec_dev |

## Bundled wake word models

Packed from the `espressif/esp-sr` 2.6.0 component at build time
(`firmware/tools/pack_srmodels.py`) and embedded in the app image. Only the
models offered in Quick Settings are packed.

| Model | Phrase | Training | License | Source |
| --- | --- | --- | --- | --- |
| `wn9_hiesp` (`wakeNet9_v1h24_Hi,ESP`) | "Hi ESP" (default) | recorded speech | ESPRESSIF MIT (use on Espressif products) | esp-sr `model/wakenet_model/wn9_hiesp` |
| `wn9_jarvis_tts` (`wakenet9l_tts1h8_Jarvis`) | "Jarvis" | TTS-generated speech | ESPRESSIF MIT (use on Espressif products) | esp-sr `model/wakenet_model/wn9_jarvis_tts` |
| `wn9_computer_tts` (`wakenet9l_tts1h8_Computer`) | "Computer" | TTS-generated speech | ESPRESSIF MIT (use on Espressif products) | esp-sr `model/wakenet_model/wn9_computer_tts` |
| `wn9_mycroft_tts` (`wakenet9l_tts1h8_Mycroft`) | "Mycroft" | TTS-generated speech | ESPRESSIF MIT (use on Espressif products) | esp-sr `model/wakenet_model/wn9_mycroft_tts` |

### microWakeWord models

Embedded from `firmware/models/mww/` (`firmware/components/kbx_wake/CMakeLists.txt`); run by
TensorFlow Lite Micro (`firmware/components/kbx_wake/wakeword_mww.cc`).

| Model | Phrase | Training | License | Source |
| --- | --- | --- | --- | --- |
| `kombify.tflite` | "Kombify" | trained by kombify with microWakeWord | kombify Wake Word Model License (source-available, not Apache-2.0; `firmware/models/mww/LICENSE-KOMBIFY-WAKE-WORDS.md`) | this tree, `firmware/models/mww/` |
| `hey_kombify.tflite` | "Hey Kombify" | trained by kombify with microWakeWord | kombify Wake Word Model License (source-available, not Apache-2.0; `firmware/models/mww/LICENSE-KOMBIFY-WAKE-WORDS.md`) | this tree, `firmware/models/mww/` |
| `hey_kubi.tflite` | "Hey Kubi" | trained by kombify with microWakeWord | kombify Wake Word Model License (source-available, not Apache-2.0; `firmware/models/mww/LICENSE-KOMBIFY-WAKE-WORDS.md`) | this tree, `firmware/models/mww/` |
| `hey_jarvis.tflite` (v2) | "Hey Jarvis" | Kevin Ahrendt, microWakeWord | Apache-2.0 | https://github.com/esphome/micro-wake-word-models (`models/v2/hey_jarvis.tflite`, commit `40ff33f`) |
| `stop.tflite` (v2) | "Stop" (stops a playing reply, RLCD 4.2) | Kevin Ahrendt, microWakeWord | Apache-2.0 | https://github.com/kahrendt/microWakeWord release `stop` (`stop.tflite`, SHA-256 `b5a18c4ad681a89950dfade31011e1631bdcb333e93c84519a1a63ff4f071146`) |

### Attribution for the data behind the own models

The kombify, Hey Kombify and Hey Kubi models were trained with
[microWakeWord](https://github.com/kahrendt/microWakeWord) (Kevin Ahrendt,
Apache-2.0, revision `4665173`) and
[piper-sample-generator](https://github.com/rhasspy/piper-sample-generator)
(Michael Hansen, MIT, revision `2971426`) on the sources below. No recording
from them is distributed; the models are derived from audio that was
synthesised, resampled, mixed with noise and room responses, and converted to
spectrogram features. The table lists every source with its URL.

| Source | Authors | Used for | License |
| --- | --- | --- | --- |
| [LibriTTS](https://www.openslr.org/60/) (through the Piper generator `en-us-libritts-high`) | Heiga Zen, Viet Dang, Rob Clark, Yu Zhang, Ron J. Weiss, Ye Jia, Zhifeng Chen, Yonghui Wu | synthetic English and German positives and negatives | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [LibriSpeech](https://www.openslr.org/12/) (train-clean-100, test-clean) | Vassil Panayotov, Guoguo Chen, Daniel Povey, Sanjeev Khudanpur | English speech negatives, held-out evaluation | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [Multilingual LibriSpeech](https://www.openslr.org/94/) (German) | Vineel Pratap, Qiantong Xu, Anuroop Sriram, Gabriel Synnaeve, Ronan Collobert | German speech negatives, validation, held-out evaluation | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [MUSAN](https://www.openslr.org/17/) | David Snyder, Guoguo Chen, Daniel Povey | noise, music and speech negatives, augmentation | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [AMI Meeting Corpus](https://groups.inf.ed.ac.uk/ami/corpus/), Mix-Headset | AMI Consortium (University of Edinburgh and partners) | meeting-audio negatives, validation, held-out evaluation | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| [RIRS_NOISES](https://www.openslr.org/28/) | Tom Ko, Vijayaditya Peddinti, Daniel Povey, Michael L. Seltzer, Sanjeev Khudanpur | room impulse responses and point-source noises | Apache-2.0 |
| [LJ Speech](https://keithito.com/LJ-Speech-Dataset/) (through Piper `en_US-ljspeech-high`) | Keith Ito, Linda Johnson | extra English speaker | public domain |
| LibriVox recordings (through Piper `en_GB-cori-high`) | LibriVox volunteers | extra British English speaker | public domain |

## Components carried in this repository

| Path | Basis | License | Modification |
| --- | --- | --- | --- |
| `firmware/components/usb_device_uac/` | `espressif/usb_device_uac` 1.3.1 | Apache-2.0 (`license.txt`) | Adds the CDC-ACM log port to the USB composite; see `KOMBIFY-PATCH.md` in that folder |
| `firmware/overlays/esp_websocket_client.c` | `espressif/esp_websocket_client` 1.7.0 | Apache-2.0 | Websocket task stack in PSRAM; see `firmware/overlays/README.md` |

## Generated font subsets

`firmware/components/kbx_ui/assets/fonts/kbx_*.c` are LVGL bitmap subsets generated by
`firmware/i18n/gen.mjs --fonts` (`lv_font_conv`, MIT) from these SIL Open Font
License 1.1 fonts, limited to the glyphs the display strings use:

| Font | Package | License |
| --- | --- | --- |
| Montserrat | `@expo-google-fonts/montserrat` 0.4.2 | OFL-1.1 |
| Noto Sans SC | `@expo-google-fonts/noto-sans-sc` 0.4.3 | OFL-1.1 |
| Noto Sans Arabic | `@expo-google-fonts/noto-sans-arabic` 0.4.3 | OFL-1.1 |

The Kombify logo assets under `firmware/components/kbx_ui/assets/` are Kombify brand
material, not third-party.
