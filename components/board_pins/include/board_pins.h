#pragma once
#include "driver/gpio.h"

#define PIN_I2C_SDA        GPIO_NUM_21
#define PIN_I2C_SCL        GPIO_NUM_22

#define PIN_MAX_SCK        GPIO_NUM_14
#define PIN_MAX_SO         GPIO_NUM_13
#define PIN_MAX_CS         GPIO_NUM_26

#define PIN_DS18B20        GPIO_NUM_4

#define PIN_SD_SCLK        GPIO_NUM_18
#define PIN_SD_MISO        GPIO_NUM_19
#define PIN_SD_MOSI        GPIO_NUM_23
#define PIN_SD_CS          GPIO_NUM_5

#define PIN_ADKEY          GPIO_NUM_34
#define PIN_TCRT5000_AO    GPIO_NUM_36

#define PIN_HEATER_SSR     GPIO_NUM_17
#define PIN_BUZZER         GPIO_NUM_25
#define PIN_FAN            GPIO_NUM_27

#define PIN_DRUM_ENA       GPIO_NUM_16
#define PIN_DRUM_IN1       GPIO_NUM_33
#define PIN_DRUM_SENSE     GPIO_NUM_35

#define PIN_FAN_TACH       GPIO_NUM_32
