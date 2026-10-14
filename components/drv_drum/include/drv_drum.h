#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct drv_drum_s *drv_drum_handle_t;

esp_err_t drv_drum_create(drv_drum_handle_t *out_handle);

void drv_drum_set(drv_drum_handle_t h, bool on);

bool drv_drum_commanded(drv_drum_handle_t h);

bool drv_drum_running(drv_drum_handle_t h);

uint8_t drv_drum_power(drv_drum_handle_t h);

uint32_t drv_drum_cycles(drv_drum_handle_t h);
