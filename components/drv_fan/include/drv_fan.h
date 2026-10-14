#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct drv_fan_s *drv_fan_handle_t;

esp_err_t drv_fan_create(drv_fan_handle_t *out_handle);

void drv_fan_set(drv_fan_handle_t h, bool on);

bool drv_fan_commanded(drv_fan_handle_t h);

bool drv_fan_spinning(drv_fan_handle_t h);

uint32_t drv_fan_rpm(drv_fan_handle_t h);

uint32_t drv_fan_cycles(drv_fan_handle_t h);
