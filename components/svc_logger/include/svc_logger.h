#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "drv_ds3231.h"

typedef struct svc_logger_s *svc_logger_handle_t;

typedef struct {
    QueueHandle_t      log_queue;
    EventGroupHandle_t events;
    drv_ds3231_handle_t rtc;
} svc_logger_config_t;

esp_err_t svc_logger_create(const svc_logger_config_t *cfg, svc_logger_handle_t *out_handle);
