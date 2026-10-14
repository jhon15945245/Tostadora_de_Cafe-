#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct drv_max6675_s *drv_max6675_handle_t;

esp_err_t drv_max6675_create(drv_max6675_handle_t *out_handle);

esp_err_t drv_max6675_read(drv_max6675_handle_t h, float *temp_c);

uint16_t drv_max6675_last_raw(drv_max6675_handle_t h);
