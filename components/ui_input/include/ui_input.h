#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "roast_types.h"
#include "drv_adkey.h"

typedef struct ui_input_s *ui_input_handle_t;

typedef struct {
    drv_adkey_handle_t keys;
    QueueHandle_t      out_queue;
    uint32_t           long_press_ms;
} ui_input_config_t;

esp_err_t ui_input_create(const ui_input_config_t *cfg, ui_input_handle_t *out_handle);

void ui_input_destroy(ui_input_handle_t handle);
