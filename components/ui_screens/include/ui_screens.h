#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "roast_types.h"
#include "drv_oled.h"
#include "svc_buzzer.h"

typedef struct ui_screens_s *ui_screens_handle_t;

typedef struct {
    QueueHandle_t       input_queue;
    QueueHandle_t       cmd_queue;
    QueueHandle_t       status_queue;
    drv_oled_handle_t   oled;
    EventGroupHandle_t  events;
    svc_buzzer_handle_t buzzer;
} ui_screens_config_t;

esp_err_t ui_screens_create(const ui_screens_config_t *cfg, ui_screens_handle_t *out_handle);
