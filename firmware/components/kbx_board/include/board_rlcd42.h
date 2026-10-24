// kombify box - Waveshare ESP32-S3-RLCD-4.2 board pins.
// Source: waveshareteam/ESP32-S3-RLCD-4.2 @ d8bf781 (examples/esp-idf/
// 07_Audio_Test user_config.h + codec_board board_cfg.txt) and the board
// schematic GPIO table; xiaozhi-esp32 boards/waveshare/esp32-s3-rlcd-4.2
// agrees on every pin.
#pragma once
#include "driver/gpio.h"

// No power latch: the PWR key drives the load switch in hardware, there is
// no keep-alive GPIO to hold.
#define BOOT_BUTTON_GPIO     GPIO_NUM_0    // active low, 10K pull-up
#define KEY_BUTTON_GPIO      GPIO_NUM_18   // active low, 10K pull-up
#define BAT_ADC_GPIO         GPIO_NUM_4    // ADC1_CH3, VBAT = Vadc * 3

// Audio is the same chip pair as the 1.85C V2 (ES7210 ADC + ES8311 DAC on
// one I2S bus). ES7210 SDOUT1 carries MIC1/MIC2 in standard I2S; MIC3 is
// the speaker loopback (AEC reference) and only appears in TDM mode.
// +7.5 dB after the AEC: the ES7210 runs the microphones at 30 dB here.
#define KBX_AEC_POST_GAIN_Q8 607
#define AUDIO_I2S_MCLK       GPIO_NUM_16
#define AUDIO_I2S_BCLK       GPIO_NUM_9
#define AUDIO_I2S_LRCK       GPIO_NUM_45
#define AUDIO_I2S_DOUT       GPIO_NUM_8
#define AUDIO_I2S_DIN        GPIO_NUM_10
#define AUDIO_PA_CTRL        GPIO_NUM_46   // NS4150B CTRL, high = on
// ES8311 at 0 dB: the NS4150B and the 8 ohm speaker need the full DAC
// swing; the 1.85C value (60 = -19.5 dB) left speech barely audible.
#define KBX_SPK_CODEC_VOLUME 75
// Listening cue ~-11 dBFS peak before volume (default ~-16): the small
// speaker needs it to be heard over the room.
#define KBX_CUE_AMP          9200.0f
// The longer bell cue until the owner picks another one (KBX sound cues style).
#define KBX_CUE_STYLE_DEFAULT AUDIO_CUE_STYLE_BELL

// I2C: ES8311 0x18, ES7210 0x40, PCF85063 RTC 0x51, SHTC3 0x70
#define SYS_I2C_SDA          GPIO_NUM_13
#define SYS_I2C_SCL          GPIO_NUM_14
#define RTC_INT_GPIO         GPIO_NUM_15

// ST7305 reflective LCD, 4-wire SPI, 1 bit per pixel, no backlight
#define LCD_SPI_HOST         SPI3_HOST
#define LCD_SCLK             GPIO_NUM_11
#define LCD_MOSI             GPIO_NUM_12
#define LCD_CS               GPIO_NUM_40
#define LCD_DC               GPIO_NUM_5
#define LCD_RST              GPIO_NUM_41
#define LCD_TE               GPIO_NUM_6
#define LCD_H_RES            400
#define LCD_V_RES            300

// TF card, SDMMC 1-bit (not used by the firmware yet)
#define SD_CLK_GPIO          GPIO_NUM_38
#define SD_CMD_GPIO          GPIO_NUM_21
#define SD_D0_GPIO           GPIO_NUM_39

#define UAC_SAMPLE_RATE      CONFIG_UAC_SAMPLE_RATE
