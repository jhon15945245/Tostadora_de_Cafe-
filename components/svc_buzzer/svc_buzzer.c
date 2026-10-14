#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#include "board_pins.h"
#include "roast_types.h"
#include "svc_buzzer.h"

static const char *TAG = "svc_buzzer";

#define BUZZER_TASK_STACK  2048
#define BUZZER_TASK_PRIO   2
#define BUZZER_TASK_CORE   1
#define BUZZER_STEP_MS     100

struct svc_buzzer_s {
    EventGroupHandle_t events;
    TaskHandle_t       task;
};

static void buzzer_task(void *arg)
{
    struct svc_buzzer_s *h = (struct svc_buzzer_s *)arg;
    uint32_t step = 0;

    while (1) {

        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUZZER_STEP_MS)) > 0) {
            gpio_set_level(PIN_BUZZER, 1);
            vTaskDelay(pdMS_TO_TICKS(60));
            gpio_set_level(PIN_BUZZER, 0);
            continue;
        }

        const EventBits_t bits = xEventGroupGetBits(h->events);
        int level = 0;
        if (bits & EVT_FAULT_ANY) {
            level = ((step % 2) == 0);
        } else if (bits & EVT_PROX) {
            level = ((step % 5) < 2);
        }
        gpio_set_level(PIN_BUZZER, level);
        step++;
    }
}

esp_err_t svc_buzzer_create(const svc_buzzer_config_t *cfg, svc_buzzer_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->events && out_handle, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    *out_handle = NULL;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUZZER,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config fallo");
    gpio_set_level(PIN_BUZZER, 0);

    struct svc_buzzer_s *h = calloc(1, sizeof(struct svc_buzzer_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->events = cfg->events;

    const BaseType_t ok = xTaskCreatePinnedToCore(buzzer_task, "buzzer", BUZZER_TASK_STACK, h,
                                                  BUZZER_TASK_PRIO, &h->task, BUZZER_TASK_CORE);
    if (ok != pdPASS) {
        free(h);
        ESP_LOGE(TAG, "no se pudo crear la tarea");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Buzzer listo (GPIO %d)", PIN_BUZZER);
    *out_handle = h;
    return ESP_OK;
}

void svc_buzzer_beep(svc_buzzer_handle_t h)
{
    if (h && h->task) {
        xTaskNotifyGive(h->task);
    }
}
