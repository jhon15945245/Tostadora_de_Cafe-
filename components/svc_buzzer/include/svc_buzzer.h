#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

typedef struct svc_buzzer_s *svc_buzzer_handle_t;

typedef struct {
    EventGroupHandle_t events;
} svc_buzzer_config_t;

esp_err_t svc_buzzer_create(const svc_buzzer_config_t *cfg, svc_buzzer_handle_t *out_handle);

void svc_buzzer_beep(svc_buzzer_handle_t h);
