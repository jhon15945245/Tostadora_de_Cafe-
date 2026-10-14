#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "roast_types.h"
#include "svc_sensor.h"

static const char *TAG = "svc_sensor";

#define SENSOR_TASK_STACK   3072
#define SENSOR_TASK_PRIO    5
#define SENSOR_TASK_CORE    1
#define SENSOR_PERIOD_MS    100
#define TEMP_EVERY_N_TICKS  3
#define TEMP_ERRORS_TO_FAIL 3
#define PROX_TICKS_TO_RAISE 2
#define PROX_TICKS_TO_CLEAR 10

struct svc_sensor_s {
    svc_sensor_config_t c;
};

#if !CONFIG_TEMP_SENSOR_DS18B20

static const char *tc_reason(uint16_t raw)
{
    if (raw == 0x0000) {
        return "SO fijo en 0: el modulo no responde (revisa VCC, GND, SCK, SO y CS)";
    }
    if (raw == 0xFFFF) {
        return "SO fijo en 1: sin alimentacion o cable SO suelto";
    }
    if (raw & 0x0004) {
        return "TERMOCUPLA ABIERTA: el modulo funciona pero la termocupla no esta conectada a sus bornes";
    }
    return "lectura invalida";
}
#endif

static void sensor_task(void *arg)
{
    struct svc_sensor_s *h = (struct svc_sensor_s *)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t tick = 0;

    float temp_c = 0.0f;
    bool  sensor_ok = false;
    int   temp_errors = 0;
#if CONFIG_TEMP_SENSOR_DS18B20
    bool     conv_started = false;
    uint32_t conv_tick = 0;
#endif

    bool  prox_alert = false;
    int   near_ticks = 0;
    int   far_ticks = 0;
    int   prox_raw = 0;

    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
        tick++;

        if (drv_tcrt5000_read(h->c.proximity, &prox_raw) == ESP_OK) {
#if CONFIG_PROX_ACTIVE_LOW
            const bool near = prox_raw < CONFIG_PROX_THRESHOLD;
#else
            const bool near = prox_raw > CONFIG_PROX_THRESHOLD;
#endif
            if (near) {
                near_ticks++;
                far_ticks = 0;
            } else {
                far_ticks++;
                near_ticks = 0;
            }
            if (!prox_alert && near_ticks >= PROX_TICKS_TO_RAISE) {
                prox_alert = true;
                xEventGroupSetBits(h->c.events, EVT_PROX);
                ESP_LOGW(TAG, "ALERTA: algo muy cerca (lectura %d)", prox_raw);
            } else if (prox_alert && far_ticks >= PROX_TICKS_TO_CLEAR) {
                prox_alert = false;
                xEventGroupClearBits(h->c.events, EVT_PROX);
                ESP_LOGI(TAG, "alerta de proximidad terminada");
            }
        }

        bool new_temp = false;
#if CONFIG_TEMP_SENSOR_DS18B20

        if (!conv_started) {
            if (drv_ds18b20_start(h->c.ds18b20) == ESP_OK) {
                conv_started = true;
                conv_tick = tick;
            } else if (++temp_errors >= TEMP_ERRORS_TO_FAIL) {
                sensor_ok = false;
            }
        } else if ((tick - conv_tick) >= 9) {
            float t = 0.0f;
            conv_started = false;
            if (drv_ds18b20_read(h->c.ds18b20, &t) == ESP_OK) {
                temp_c = t;
                sensor_ok = true;
                temp_errors = 0;
            } else if (++temp_errors >= TEMP_ERRORS_TO_FAIL) {
                if (sensor_ok) {
                    ESP_LOGE(TAG, "DS18B20 sin lectura valida: %s", drv_ds18b20_last_error(h->c.ds18b20));
                }
                sensor_ok = false;
            }
            new_temp = true;
        }
#else

        if ((tick % TEMP_EVERY_N_TICKS) == 1) {
            float t = 0.0f;
            if (drv_max6675_read(h->c.thermocouple, &t) == ESP_OK) {
                temp_c = t;
                sensor_ok = true;
                temp_errors = 0;
            } else if (++temp_errors >= TEMP_ERRORS_TO_FAIL) {
                if (sensor_ok) {
                    ESP_LOGE(TAG, "termocupla sin lectura valida (abierta o desconectada)");
                }
                sensor_ok = false;
            }
            new_temp = true;
        }
#endif

#if CONFIG_PROX_DEBUG_LOG
        if ((tick % 20) == 0) {
#if CONFIG_TEMP_SENSOR_DS18B20
            ESP_LOGI(TAG, "temp=%.2f C (%s) [DS18B20] | prox_raw=%d | alerta=%d",
                     (double)temp_c, sensor_ok ? "ok" : "FALLA", prox_raw, (int)prox_alert);
            if (!sensor_ok) {
                ESP_LOGW(TAG, "DS18B20: %s", drv_ds18b20_last_error(h->c.ds18b20));
            }
#else
            const uint16_t traw = drv_max6675_last_raw(h->c.thermocouple);
            ESP_LOGI(TAG, "temp=%.2f C (%s) | max6675 raw=0x%04X | prox_raw=%d | alerta=%d",
                     (double)temp_c, sensor_ok ? "ok" : "FALLA", (unsigned)traw, prox_raw, (int)prox_alert);
            if (!sensor_ok) {
                ESP_LOGW(TAG, "termocupla: %s", tc_reason(traw));
            }
#endif
        }
#endif

        const sensor_sample_t s = {
            .t_ms       = (uint32_t)(esp_timer_get_time() / 1000),
            .temp_c     = temp_c,
            .sensor_ok  = sensor_ok,
            .prox_raw   = (uint16_t)prox_raw,
            .prox_alert = prox_alert,
        };
        xQueueOverwrite(h->c.sample_queue, &s);
        if (new_temp) {
            xSemaphoreGive(h->c.sample_sem);
        }
    }
}

esp_err_t svc_sensor_create(const svc_sensor_config_t *cfg, svc_sensor_handle_t *out_handle)
{
#if CONFIG_TEMP_SENSOR_DS18B20
    ESP_RETURN_ON_FALSE(cfg && cfg->ds18b20 && cfg->proximity && cfg->sample_queue &&
                        cfg->sample_sem && cfg->events && out_handle,
                        ESP_ERR_INVALID_ARG, TAG, "argumento nulo (falta el DS18B20)");
#else
    ESP_RETURN_ON_FALSE(cfg && cfg->thermocouple && cfg->proximity && cfg->sample_queue &&
                        cfg->sample_sem && cfg->events && out_handle,
                        ESP_ERR_INVALID_ARG, TAG, "argumento nulo (falta el MAX6675)");
#endif
    *out_handle = NULL;

    struct svc_sensor_s *h = calloc(1, sizeof(struct svc_sensor_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->c = *cfg;

    const BaseType_t ok = xTaskCreatePinnedToCore(sensor_task, "sensor", SENSOR_TASK_STACK, h,
                                                  SENSOR_TASK_PRIO, NULL, SENSOR_TASK_CORE);
    if (ok != pdPASS) {
        free(h);
        ESP_LOGE(TAG, "no se pudo crear la tarea");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Sensores listos");
    *out_handle = h;
    return ESP_OK;
}
