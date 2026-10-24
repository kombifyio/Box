// kombify box - Waveshare ESP32-S3-Touch-LCD-1.85C board pins
#pragma once
#include "driver/gpio.h"

#define PWR_BUTTON_GPIO      GPIO_NUM_6
#define PWR_CONTROL_GPIO     GPIO_NUM_7   // keep high to stay powered from battery
#define BOOT_BUTTON_GPIO     GPIO_NUM_0

// Shared V2 audio bus (ES7210 ADC + ES8311 DAC)
#define AUDIO_I2S_MCLK       GPIO_NUM_2
#define AUDIO_I2S_BCLK       GPIO_NUM_48
#define AUDIO_I2S_LRCK       GPIO_NUM_38
#define AUDIO_I2S_DOUT       GPIO_NUM_47
#define AUDIO_I2S_DIN        GPIO_NUM_39
#define AUDIO_PA_CTRL        GPIO_NUM_15

// I2C (TCA9554 io expander, RTC, IMU) - I2C port 0
#define SYS_I2C_SDA          GPIO_NUM_11
#define SYS_I2C_SCL          GPIO_NUM_10

// Touch CST816S: on the 1.85C it sits on the SHARED system I2C (SDA11/
// SCL10, together with TCA9554/codecs) — no dedicated bus. SDA1/SCL3 was
// the pinout of the non-C 1.85 board (chip never answered there; verified
// against xiaozhi-esp32 boards/waveshare/esp32-s3-touch-lcd-1.85c config.h:
// TP_PIN_NUM_SDA=11, SCL=10, INT=4, RST=NC, 2026-07-11).
#define TP_INT_GPIO          GPIO_NUM_4

// QSPI LCD ST77916 360x360
#define LCD_QSPI_HOST        SPI2_HOST
#define LCD_PCLK             GPIO_NUM_40
#define LCD_CS               GPIO_NUM_21
#define LCD_DATA0            GPIO_NUM_46
#define LCD_DATA1            GPIO_NUM_45
#define LCD_DATA2            GPIO_NUM_42
#define LCD_DATA3            GPIO_NUM_41
#define LCD_BACKLIGHT        GPIO_NUM_5
#define LCD_H_RES            360
#define LCD_V_RES            360

#define UAC_SAMPLE_RATE      CONFIG_UAC_SAMPLE_RATE
