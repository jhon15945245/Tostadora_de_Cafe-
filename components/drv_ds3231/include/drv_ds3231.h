#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef struct drv_ds3231_s *drv_ds3231_handle_t;

typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} rtc_datetime_t;

esp_err_t drv_ds3231_create(i2c_master_bus_handle_t bus, drv_ds3231_handle_t *out_handle);

esp_err_t drv_ds3231_get(drv_ds3231_handle_t h, rtc_datetime_t *dt);

esp_err_t drv_ds3231_set(drv_ds3231_handle_t h, const rtc_datetime_t *dt);

esp_err_t drv_ds3231_lost_power(drv_ds3231_handle_t h, bool *lost);
