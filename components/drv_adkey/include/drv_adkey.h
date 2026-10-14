#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "roast_types.h"

typedef struct drv_adkey_s *drv_adkey_handle_t;

typedef struct {
    adc_oneshot_unit_handle_t adc;
    uint8_t samples;
    uint8_t debounce;
} drv_adkey_config_t;

esp_err_t drv_adkey_create(const drv_adkey_config_t *cfg, drv_adkey_handle_t *out_handle);

esp_err_t drv_adkey_poll(drv_adkey_handle_t handle, input_key_t *key_out, int *raw_out);

void drv_adkey_destroy(drv_adkey_handle_t handle);
