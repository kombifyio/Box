# Audio base: I2S capture and playback, USB audio, DSP and echo cancellation.
# Codec drivers are selected by the profile's codec capabilities.
set(kbx_audio_srcs "audio.c" "pcm_dsp.c" "aec_stage.c")
if(CONFIG_KBX_AUDIO_CODEC_ES8311)
    list(APPEND kbx_audio_srcs "es8311_min.c")
endif()
if(CONFIG_KBX_AUDIO_ADC_ES7210)
    list(APPEND kbx_audio_srcs "es7210_min.c")
endif()

idf_component_register(
    SRCS ${kbx_audio_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES kbx_board esp_driver_gpio esp_driver_i2s esp_ringbuf esp_timer nvs_flash
                  usb_device_uac espressif__esp-sr
)
