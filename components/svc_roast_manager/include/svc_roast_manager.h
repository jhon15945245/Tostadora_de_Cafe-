#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "roast_types.h"
#include "drv_heater.h"
#include "drv_fan.h"
#include "drv_drum.h"

typedef struct svc_roast_manager_s *svc_roast_manager_handle_t;

typedef struct {
    QueueHandle_t      cmd_queue;
    QueueHandle_t      status_queue;
    QueueHandle_t      sample_queue;
    SemaphoreHandle_t  sample_sem;
    QueueHandle_t      log_queue;
    EventGroupHandle_t events;
    drv_heater_handle_t heater;
    drv_fan_handle_t    fan;
    drv_drum_handle_t   drum;
} svc_roast_manager_config_t;

esp_err_t svc_roast_manager_create(const svc_roast_manager_config_t *cfg,
                                   svc_roast_manager_handle_t *out_handle);
