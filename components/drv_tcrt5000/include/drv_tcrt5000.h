#pragma once

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

typedef struct drv_tcrt5000_s *drv_tcrt5000_handle_t;

esp_err_t drv_tcrt5000_create(adc_oneshot_unit_handle_t adc, drv_tcrt5000_handle_t *out_handle);

esp_err_t drv_tcrt5000_read(drv_tcrt5000_handle_t h, int *raw_out);
