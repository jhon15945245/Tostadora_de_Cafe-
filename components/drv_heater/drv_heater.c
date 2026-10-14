#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#include "board_pins.h"
#include "drv_heater.h"

static const char *TAG = "drv_heater";

#define HEATER_SLOT_MS      100
#define HEATER_DEADMAN_MS   2000
#define HEATER_TASK_STACK   2048
#define HEATER_TASK_PRIO    6
#define HEATER_TASK_CORE    1

#if CONFIG_HEATER_OUT_SSR
#define HEATER_ACTIVE_LOW    0
#define HEATER_WINDOW_SLOTS  20
#define HEATER_MIN_SLOTS     0
#define HEATER_KIND          "SSR activo en alto, ciclo 2 s"
#elif CONFIG_HEATER_OUT_RELAY_ACTIVE_LOW
#define HEATER_ACTIVE_LOW    1
#define HEATER_WINDOW_SLOTS  (CONFIG_HEATER_RELAY_WINDOW_S * 1000 / HEATER_SLOT_MS)
#define HEATER_MIN_SLOTS     (CONFIG_HEATER_RELAY_MIN_S * 1000 / HEATER_SLOT_MS)
#define HEATER_KIND          "rele ACTIVO EN BAJO"
#else
#define HEATER_ACTIVE_LOW    0
#define HEATER_WINDOW_SLOTS  (CONFIG_HEATER_RELAY_WINDOW_S * 1000 / HEATER_SLOT_MS)
#define HEATER_MIN_SLOTS     (CONFIG_HEATER_RELAY_MIN_S * 1000 / HEATER_SLOT_MS)
#define HEATER_KIND          "rele ACTIVO EN ALTO"
#endif

#if CONFIG_HEATER_OPEN_DRAIN
#define HEATER_PIN_MODE      GPIO_MODE_OUTPUT_OD
#else
#define HEATER_PIN_MODE      GPIO_MODE_OUTPUT
#endif

#define HEATER_OFF_LEVEL     (HEATER_ACTIVE_LOW ? 1 : 0)

static portMUX_TYPE s_pin_mux = portMUX_INITIALIZER_UNLOCKED;

struct drv_heater_s {
    SemaphoreHandle_t mtx;
    uint8_t           pct;
    TickType_t        last_set;
    bool              pin_on;
    uint32_t          on_count;
};

static void heater_apply(drv_heater_handle_t h, bool on)
{
#if CONFIG_BENCH_SIMULATE_SENSOR
    on = false;
#endif
    portENTER_CRITICAL(&s_pin_mux);
    if (on && !h->pin_on) {
        h->on_count++;
    }
    h->pin_on = on;
    gpio_set_level(PIN_HEATER_SSR, (on ? !HEATER_OFF_LEVEL : HEATER_OFF_LEVEL) ? 1 : 0);
    portEXIT_CRITICAL(&s_pin_mux);
}

static void heater_task(void *arg)
{
    drv_heater_handle_t h = (drv_heater_handle_t)arg;
    TickType_t last = xTaskGetTickCount();
    uint16_t slot = 0;
    unsigned on_slots = 0;

    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(HEATER_SLOT_MS));

        uint8_t pct = 0;
        if (xSemaphoreTake(h->mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            if ((xTaskGetTickCount() - h->last_set) <= pdMS_TO_TICKS(HEATER_DEADMAN_MS)) {
                pct = h->pct;
            }
            xSemaphoreGive(h->mtx);
        }

        if (slot == 0) {
            unsigned n = ((unsigned)pct * HEATER_WINDOW_SLOTS + 50u) / 100u;
#if HEATER_MIN_SLOTS > 0
            if (n < HEATER_MIN_SLOTS) {
                n = 0;
            } else if ((HEATER_WINDOW_SLOTS - n) < HEATER_MIN_SLOTS) {
                n = HEATER_WINDOW_SLOTS;
            }
#endif
            on_slots = n;
        }

        heater_apply(h, (pct > 0) && (slot < on_slots));
        slot = (uint16_t)((slot + 1) % HEATER_WINDOW_SLOTS);
    }
}

esp_err_t drv_heater_create(drv_heater_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "out_handle nulo");
    *out_handle = NULL;

    gpio_set_level(PIN_HEATER_SSR, HEATER_OFF_LEVEL);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_HEATER_SSR,
        .mode         = HEATER_PIN_MODE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config fallo");
    gpio_set_level(PIN_HEATER_SSR, HEATER_OFF_LEVEL);

    drv_heater_handle_t h = calloc(1, sizeof(struct drv_heater_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");

    h->mtx = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(h->mtx, ESP_ERR_NO_MEM, TAG, "no se pudo crear el mutex");

    const BaseType_t ok = xTaskCreatePinnedToCore(heater_task, "heater", HEATER_TASK_STACK, h,
                                                  HEATER_TASK_PRIO, NULL, HEATER_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "no se pudo crear la tarea");

    ESP_LOGI(TAG, "Calentador listo, APAGADO (%s, GPIO %d, ciclo %d s, hombre muerto 2 s)",
             HEATER_KIND, PIN_HEATER_SSR, HEATER_WINDOW_SLOTS * HEATER_SLOT_MS / 1000);
#if CONFIG_HEATER_OUT_RELAY_ACTIVE_LOW || CONFIG_HEATER_OUT_RELAY_ACTIVE_HIGH
    ESP_LOGW(TAG, "Un rele mecanico puede quedar pegado: el fusible termico en serie es obligatorio");
#endif
    *out_handle = h;
    return ESP_OK;
}

void drv_heater_set(drv_heater_handle_t h, uint8_t pct)
{
    if (!h) {
        return;
    }
    if (pct > 100) {
        pct = 100;
    }
    if (xSemaphoreTake(h->mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        h->pct = pct;
        h->last_set = xTaskGetTickCount();
        xSemaphoreGive(h->mtx);
    }
}

void drv_heater_off(drv_heater_handle_t h)
{
    if (!h) {
        return;
    }
    heater_apply(h, false);
    if (xSemaphoreTake(h->mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        h->pct = 0;
        xSemaphoreGive(h->mtx);
    }
}

uint32_t drv_heater_cycles(drv_heater_handle_t h)
{
    uint32_t n = 0;
    if (h) {
        portENTER_CRITICAL(&s_pin_mux);
        n = h->on_count;
        portEXIT_CRITICAL(&s_pin_mux);
    }
    return n;
}
