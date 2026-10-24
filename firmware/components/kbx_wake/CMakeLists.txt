# Wake word engines (ESP-SR WakeNet, microWakeWord), the on-board stop word
# and the candidate hand-off for server verification (two-stage wake word).
idf_component_register(
    SRCS "wakeword.c" "wakeword_mww.cc" "stop_word.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    PRIV_REQUIRES kbx_audio kbx_core kbx_voice esp_timer nvs_flash
                  espressif__esp-sr espressif__esp-tflite-micro esphome__esp-micro-speech-features
)

idf_build_get_property(kbx_project_dir PROJECT_DIR)

# Wake word models: pack the offered models from the esp-sr component in esp-sr's
# srmodels format and embed it in the app image (no model partition).
# tools/pack_srmodels.py starts every file on a 16-byte boundary and the
# assembly stub below places the pack itself on one. target_add_binary_data
# gives no alignment: dev8's pack landed on an odd address, so WakeNet's
# index and weights were read misaligned when the engine was created.
idf_component_get_property(kbx_sr_dir espressif__esp-sr COMPONENT_DIR)
idf_build_get_property(kbx_python PYTHON)
# Offered phrases only (wakeword.c k_phrases): "Hi ESP", "Jarvis",
# "Computer", "Mycroft" (~291 KB each; only the selected one is built).
set(kbx_sr_models
    "${kbx_sr_dir}/model/wakenet_model/wn9_hiesp"
    "${kbx_sr_dir}/model/wakenet_model/wn9_jarvis_tts"
    "${kbx_sr_dir}/model/wakenet_model/wn9_computer_tts"
    "${kbx_sr_dir}/model/wakenet_model/wn9_mycroft_tts")
set(kbx_sr_model_files)
foreach(kbx_m ${kbx_sr_models})
    list(APPEND kbx_sr_model_files "${kbx_m}/wn9_data" "${kbx_m}/wn9_index" "${kbx_m}/_MODEL_INFO_")
endforeach()
set(kbx_sr_packer "${kbx_project_dir}/tools/pack_srmodels.py")
set(kbx_srmodels "${CMAKE_CURRENT_BINARY_DIR}/kbx_srmodels.bin")
set(kbx_srmodels_asm "${CMAKE_CURRENT_BINARY_DIR}/kbx_srmodels.S")
add_custom_command(OUTPUT "${kbx_srmodels}"
    COMMAND ${kbx_python} "${kbx_sr_packer}" ${kbx_sr_models} -o "${kbx_srmodels}"
    DEPENDS "${kbx_sr_packer}" ${kbx_sr_model_files}
    COMMENT "Packing wake word models (16-byte aligned)"
    VERBATIM)
add_custom_target(kbx_srmodels_bin DEPENDS "${kbx_srmodels}")
file(CONFIGURE OUTPUT "${kbx_srmodels_asm}" CONTENT [=[
    .section .rodata.kbx_srmodels, "a"
    .balign 16
    .global kbx_srmodels_start
kbx_srmodels_start:
    .incbin "${kbx_srmodels}"
    .global kbx_srmodels_end
kbx_srmodels_end:
]=])
target_sources(${COMPONENT_LIB} PRIVATE "${kbx_srmodels_asm}")
set_source_files_properties("${kbx_srmodels_asm}" PROPERTIES OBJECT_DEPENDS "${kbx_srmodels}")
add_dependencies(${COMPONENT_LIB} kbx_srmodels_bin)

# microWakeWord models (wakeword.c k_mww): the .tflite flatbuffers in
# firmware/models/mww, each on a 16-byte boundary in the app image. The
# kombify, hey_kombify and hey_kubi models are copies of
# wakeword-training/models/<id>/ (keep them in sync when a model is retrained);
# hey_jarvis is esphome/micro-wake-word-models v2 (Apache-2.0); stop is the
# kahrendt/microWakeWord "stop" release (Apache-2.0), run by stop_word.c.
set(kbx_mww_dir "${kbx_project_dir}/models/mww")
set(kbx_mww_files "${kbx_mww_dir}/kombify.tflite" "${kbx_mww_dir}/hey_kombify.tflite"
    "${kbx_mww_dir}/hey_kubi.tflite" "${kbx_mww_dir}/hey_jarvis.tflite" "${kbx_mww_dir}/stop.tflite")
set(kbx_mww_asm "${CMAKE_CURRENT_BINARY_DIR}/kbx_mww_models.S")
file(CONFIGURE OUTPUT "${kbx_mww_asm}" CONTENT [=[
    .section .rodata.kbx_mww, "a"
    .balign 16
    .global kbx_mww_kombify_start
kbx_mww_kombify_start:
    .incbin "${kbx_mww_dir}/kombify.tflite"
    .global kbx_mww_kombify_end
kbx_mww_kombify_end:
    .balign 16
    .global kbx_mww_hey_kombify_start
kbx_mww_hey_kombify_start:
    .incbin "${kbx_mww_dir}/hey_kombify.tflite"
    .global kbx_mww_hey_kombify_end
kbx_mww_hey_kombify_end:
    .balign 16
    .global kbx_mww_hey_kubi_start
kbx_mww_hey_kubi_start:
    .incbin "${kbx_mww_dir}/hey_kubi.tflite"
    .global kbx_mww_hey_kubi_end
kbx_mww_hey_kubi_end:
    .balign 16
    .global kbx_mww_hey_jarvis_start
kbx_mww_hey_jarvis_start:
    .incbin "${kbx_mww_dir}/hey_jarvis.tflite"
    .global kbx_mww_hey_jarvis_end
kbx_mww_hey_jarvis_end:
    .balign 16
    .global kbx_mww_stop_start
kbx_mww_stop_start:
    .incbin "${kbx_mww_dir}/stop.tflite"
    .global kbx_mww_stop_end
kbx_mww_stop_end:
]=])
target_sources(${COMPONENT_LIB} PRIVATE "${kbx_mww_asm}")
set_source_files_properties("${kbx_mww_asm}" PROPERTIES OBJECT_DEPENDS "${kbx_mww_files}")
