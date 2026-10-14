#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "drv_fan.h"

static const char *TAG = "drv_fan";

#define FAN_TASK_STACK  2048
#define FAN_TASK_PRIO   3
#define FAN_TASK_CORE   1
#define FAN_WINDOW_MS   1000

#if CONFIG_FAN_OUT_RELAY_ACTIVE_LOW
#define FAN_ACTIVE_LOW  1
#define FAN_KIND        "rele ACTIVO EN BAJO"
#elif CONFIG_FAN_OUT_RELAY_ACTIVE_HIGH
#define FAN_ACTIVE_LOW  0
#define FAN_KIND        "rele ACTIVO EN ALTO"
#else
#define FAN_ACTIVE_LOW  0
#define FAN_KIND        "modulo MOSFET"
#endif
#define FAN_OFF_LEVEL   (FAN_ACTIVE_LOW ? 1 : 0)

#if CONFIG_FAN_OPEN_DRAIN
#define FAN_PIN_MODE    GPIO_MODE_OUTPUT_OD
#else
#define FAN_PIN_MODE    GPIO_MODE_OUTPUT
#endif

#if CONFIG_FAN_TACH_PULSES
#define FAN_PPR         CONFIG_FAN_TACH_PPR
#define FAN_MIN_RPM     CONFIG_FAN_MIN_RPM
#define FAN_MAX_RPM     CONFIG_FAN_MAX_RPM
#else
#define FAN_PPR         2
#define FAN_MIN_RPM     0
#define FAN_MAX_RPM     1000000
#endif

#define FAN_MIN_PULSE_US 1500

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

struct drv_fan_s {
    volatile uint32_t pulses;
    uint32_t          last_count;
    uint32_t          last_pulse_us;
    uint32_t          pps;
    TickType_t        pps_tick;
    TickType_t        on_tick;
    bool              commanded;
    uint32_t          on_count;
    int               log_div;
};

#if CONFIG_FAN_TACH_PULSES
static void IRAM_ATTR tach_isr(void *arg)
{
    struct drv_fan_s *h = (struct drv_fan_s *)arg;
    const uint32_t now_us = (uint32_t)esp_timer_get_time();
    if ((uint32_t)(now_us - h->last_pulse_us) < FAN_MIN_PULSE_US) {
        return;
    }
    h->last_pulse_us = now_us;
    h->pulses++;
}
#endif

static void fan_pin(bool on)
{
    gpio_set_level(PIN_FAN, (on ? !FAN_OFF_LEVEL : FAN_OFF_LEVEL) ? 1 : 0);
}

static void fan_task(void *arg)
{
    drv_fan_handle_t h = (drv_fan_handle_t)arg;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FAN_WINDOW_MS));

        const uint32_t count = h->pulses;
        const uint32_t d = count - h->last_count;
        h->last_count = count;

        bool on;
        portENTER_CRITICAL(&s_mux);
        h->pps = d;
        h->pps_tick = xTaskGetTickCount();
        on = h->commanded;
        portEXIT_CRITICAL(&s_mux);

#if CONFIG_FAN_TACH_PULSES
        if (on && (d * 60u / FAN_PPR) > FAN_MAX_RPM) {
            ESP_LOGW(TAG, "%u pulsos/s => %u rpm es IMPOSIBLE para este ventilador: ruido en el cable amarillo "
                          "(cable suelto o sin resistencia de 10 kohm). No se cuenta como giro.",
                     (unsigned)d, (unsigned)(d * 60u / FAN_PPR));
        } else if (on && (++h->log_div % 2) == 0) {
            ESP_LOGI(TAG, "ventilador encendido: %u pulsos/s => ~%u rpm (con %d pulsos por vuelta)",
                     (unsigned)d, (unsigned)(d * 60u / FAN_PPR), FAN_PPR);
        }
#else
        (void)on;
#endif
    }
}

esp_err_t drv_fan_create(drv_fan_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "out_handle nulo");
    *out_handle = NULL;

    gpio_set_level(PIN_FAN, FAN_OFF_LEVEL);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_FAN,
        .mode         = FAN_PIN_MODE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config de la salida fallo");
    gpio_set_level(PIN_FAN, FAN_OFF_LEVEL);

    drv_fan_handle_t h = calloc(1, sizeof(struct drv_fan_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");

#if CONFIG_FAN_TACH_PULSES
    const gpio_config_t tach = {
        .pin_bit_mask = 1ULL << PIN_FAN_TACH,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&tach), TAG, "gpio_config del tacometro fallo");
    const esp_err_t r = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(r == ESP_OK || r == ESP_ERR_INVALID_STATE, r, TAG, "gpio_install_isr_service fallo");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_FAN_TACH, tach_isr, h), TAG, "no se pudo activar la interrupcion");
#endif

    const BaseType_t ok = xTaskCreatePinnedToCore(fan_task, "fan", FAN_TASK_STACK, h,
                                                  FAN_TASK_PRIO, NULL, FAN_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "no se pudo crear la tarea");

    ESP_LOGI(TAG, "Ventilador listo, APAGADO (%s en GPIO %d, tacometro %s)", FAN_KIND, PIN_FAN,
#if CONFIG_FAN_TACH_PULSES
             "en GPIO 32: se comprueba el giro"
#else
             "NO se usa: no se comprueba el giro"
#endif
    );
    *out_handle = h;
    return ESP_OK;
}

void drv_fan_set(drv_fan_handle_t h, bool on)
{
    if (!h) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (on && !h->commanded) {
        h->on_tick = xTaskGetTickCount();
        h->on_count++;
        h->pps = 0;
    }
    h->commanded = on;
    portEXIT_CRITICAL(&s_mux);
    fan_pin(on);
}

bool drv_fan_commanded(drv_fan_handle_t h)
{
    bool c = false;
    if (h) {
        portENTER_CRITICAL(&s_mux);
        c = h->commanded;
        portEXIT_CRITICAL(&s_mux);
    }
    return c;
}

bool drv_fan_spinning(drv_fan_handle_t h)
{
    bool ok = false;
    if (!h) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    if (h->commanded) {
        const TickType_t now = xTaskGetTickCount();
        const TickType_t ready_at = h->on_tick + pdMS_TO_TICKS(CONFIG_FAN_SPINUP_MS);
#if CONFIG_FAN_TACH_PULSES

        const bool window_ok  = (int32_t)(h->pps_tick - ready_at) >= 0;
        const bool window_new = (now - h->pps_tick) <= pdMS_TO_TICKS(2500);
        const uint32_t rpm = h->pps * 60u / FAN_PPR;
        ok = window_ok && window_new && (rpm >= FAN_MIN_RPM) && (rpm <= FAN_MAX_RPM);
#else
        ok = (int32_t)(now - ready_at) >= 0;
#endif
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

uint32_t drv_fan_rpm(drv_fan_handle_t h)
{
    uint32_t rpm = 0;
#if CONFIG_FAN_TACH_PULSES
    if (h) {
        portENTER_CRITICAL(&s_mux);
        if (h->commanded) {
            rpm = h->pps * 60u / FAN_PPR;
            if (rpm > FAN_MAX_RPM) {
                rpm = 0;
            }
        }
        portEXIT_CRITICAL(&s_mux);
    }
#else
    (void)h;
#endif
    return rpm;
}

uint32_t drv_fan_cycles(drv_fan_handle_t h)
{
    uint32_t n = 0;
    if (h) {
        portENTER_CRITICAL(&s_mux);
        n = h->on_count;
        portEXIT_CRITICAL(&s_mux);
    }
    return n;
}
