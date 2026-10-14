#pragma once

#include "esp_err.h"

typedef struct drv_ds18b20_s *drv_ds18b20_handle_t;

esp_err_t drv_ds18b20_create(drv_ds18b20_handle_t *out_handle);

esp_err_t drv_ds18b20_start(drv_ds18b20_handle_t h);

esp_err_t drv_ds18b20_read(drv_ds18b20_handle_t h, float *temp_c);

const char *drv_ds18b20_last_error(drv_ds18b20_handle_t h);
