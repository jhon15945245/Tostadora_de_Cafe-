#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "drv_max6675.h"
#include "drv_ds18b20.h"
#include "drv_tcrt5000.h"

typedef struct svc_sensor_s *svc_sensor_handle_t;

typedef struct {
    drv_max6675_handle_t  thermocouple;
    drv_ds18b20_handle_t  ds18b20;
    drv_tcrt5000_handle_t proximity;
    QueueHandle_t         sample_queue;
    SemaphoreHandle_t     sample_sem;
    EventGroupHandle_t    events;
} svc_sensor_config_t;

esp_err_t svc_sensor_create(const svc_sensor_config_t *cfg, svc_sensor_handle_t *out_handle);
