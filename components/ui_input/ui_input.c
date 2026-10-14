#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"

#include "ui_input.h"

static const char *TAG = "ui_input";

#define UI_INPUT_POLL_MS          10
#define UI_INPUT_DEFAULT_LONG_MS  800
#define UI_INPUT_TASK_STACK       3072
#define UI_INPUT_TASK_PRIO        3
#define UI_INPUT_TASK_CORE        0

struct ui_input_s {
    drv_adkey_handle_t keys;
    QueueHandle_t      queue;
    TickType_t         long_ticks;
    TaskHandle_t       task;
    SemaphoreHandle_t  done;
    volatile bool      stop;
};

static void post_event(ui_input_handle_t h, input_key_t key, input_press_t press)
{
    const input_event_t ev = { .key = key, .press = press };
    if (xQueueSend(h->queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cola llena: evento perdido");
    }
}

static void input_task(void *arg)
{
    ui_input_handle_t h = (ui_input_handle_t)arg;

    input_key_t prev = INPUT_KEY_NONE;
    TickType_t  t_press = 0;
    bool        long_sent = false;
    bool        error_logged = false;
    TickType_t  xLastWake = xTaskGetTickCount();

    while (!h->stop) {
        input_key_t key = INPUT_KEY_NONE;

        if (drv_adkey_poll(h->keys, &key, NULL) == ESP_OK) {
            const TickType_t now = xTaskGetTickCount();

            if (key != prev) {

                if (prev != INPUT_KEY_NONE && !long_sent) {
                    post_event(h, prev, INPUT_PRESS_SHORT);
                }

                if (key != INPUT_KEY_NONE) {
                    t_press = now;
                    long_sent = false;
                }
                prev = key;
            } else if (key != INPUT_KEY_NONE && !long_sent &&
                       (now - t_press) >= h->long_ticks) {
                post_event(h, key, INPUT_PRESS_LONG);
                long_sent = true;
            }
        } else if (!error_logged) {
            ESP_LOGE(TAG, "fallo al leer el teclado");
            error_logged = true;
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(UI_INPUT_POLL_MS));
    }

    xSemaphoreGive(h->done);
    vTaskDelete(NULL);
}

esp_err_t ui_input_create(const ui_input_config_t *cfg, ui_input_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->keys && cfg->out_queue && out_handle,
                        ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    *out_handle = NULL;

    esp_err_t ret = ESP_OK;
    ui_input_handle_t h = calloc(1, sizeof(struct ui_input_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");

    const uint32_t long_ms = cfg->long_press_ms ? cfg->long_press_ms : UI_INPUT_DEFAULT_LONG_MS;
    h->keys       = cfg->keys;
    h->queue      = cfg->out_queue;
    h->long_ticks = pdMS_TO_TICKS(long_ms);

    h->done = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(h->done, ESP_ERR_NO_MEM, err, TAG, "no se pudo crear el semaforo");

    BaseType_t ok = xTaskCreatePinnedToCore(input_task, "ui_input", UI_INPUT_TASK_STACK, h,
                                            UI_INPUT_TASK_PRIO, &h->task, UI_INPUT_TASK_CORE);
    ESP_GOTO_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, err, TAG, "no se pudo crear la tarea");

    ESP_LOGI(TAG, "Entrada lista (pulsacion larga = %u ms)", (unsigned)long_ms);
    *out_handle = h;
    return ESP_OK;

err:
    ui_input_destroy(h);
    return ret;
}

void ui_input_destroy(ui_input_handle_t h)
{
    if (!h) {
        return;
    }
    if (h->task) {
        h->stop = true;
        xSemaphoreTake(h->done, portMAX_DELAY);
    }
    if (h->done) {
        vSemaphoreDelete(h->done);
    }
    free(h);
}
