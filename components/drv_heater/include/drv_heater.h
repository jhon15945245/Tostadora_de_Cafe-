#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct drv_heater_s *drv_heater_handle_t;

esp_err_t drv_heater_create(drv_heater_handle_t *out_handle);

void drv_heater_set(drv_heater_handle_t h, uint8_t pct);

void drv_heater_off(drv_heater_handle_t h);

uint32_t drv_heater_cycles(drv_heater_handle_t h);
