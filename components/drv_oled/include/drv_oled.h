#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define DRV_OLED_W 128
#define DRV_OLED_H 64

typedef struct drv_oled_s *drv_oled_handle_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    uint8_t                 addr;
    uint8_t                 col_offset;
} drv_oled_config_t;

esp_err_t drv_oled_create(const drv_oled_config_t *cfg, drv_oled_handle_t *out_handle);

void drv_oled_clear(drv_oled_handle_t h);

void drv_oled_text(drv_oled_handle_t h, int x, int y, const char *s, uint8_t scale, bool inverted);

void drv_oled_fill_rect(drv_oled_handle_t h, int x, int y, int w, int hgt, bool on);
void drv_oled_rect(drv_oled_handle_t h, int x, int y, int w, int hgt);

esp_err_t drv_oled_flush(drv_oled_handle_t h);
