#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

#include "board_pins.h"
#include "drv_drum.h"

static const char *TAG = "drv_drum";

#define DRUM_TASK_STACK  2048
#define DRUM_TASK_PRIO   3
#define DRUM_TASK_CORE   1
#define DRUM_STEP_MS     20

#define DRUM_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define DRUM_LEDC_TIMER  LEDC_TIMER_1
#define DRUM_LEDC_CH     LEDC_CHANNEL_1
#define DRUM_LEDC_BITS   LEDC_TIMER_10_BIT
#define DRUM_DUTY_MAX    ((1u << 10) - 1u)

struct drv_drum_s {
    SemaphoreHandle_t lock;
    bool       commanded;
    uint8_t    power;
    TickType_t on_tick;
    uint32_t   on_count;
};

static void apply_locked(drv_drum_handle_t h, uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    h->power = pct;
    gpio_set_level(PIN_DRUM_IN1, pct > 0 ? 1 : 0);
    ledc_set_duty(DRUM_LEDC_MODE, DRUM_LEDC_CH, (uint32_t)pct * DRUM_DUTY_MAX / 100u);
    ledc_update_duty(DRUM_LEDC_MODE, DRUM_LEDC_CH);
}

static uint8_t profile_power(uint32_t elapsed_ms)
{
    const int kick  = CONFIG_DRUM_KICK_PCT;
    const int speed = CONFIG_DRUM_SPEED_PCT;
    if (elapsed_ms < (uint32_t)CONFIG_DRUM_KICK_MS) {
        return (uint8_t)kick;
    }
    const uint32_t r = elapsed_ms - (uint32_t)CONFIG_DRUM_KICK_MS;
    if (r >= (uint32_t)CONFIG_DRUM_RAMP_MS) {
        return (uint8_t)speed;
    }
    return (uint8_t)(kick + (speed - kick) * (int)r / CONFIG_DRUM_RAMP_MS);
}

static void drum_task(void *arg)
{
    drv_drum_handle_t h = (drv_drum_handle_t)arg;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(DRUM_STEP_MS));
        xSemaphoreTake(h->lock, portMAX_DELAY);
        if (h->commanded) {
            const uint32_t ms = (uint32_t)(xTaskGetTickCount() - h->on_tick) * portTICK_PERIOD_MS;
            const uint8_t p = profile_power(ms);
            if (p != h->power) {
                apply_locked(h, p);
            }
        }
        xSemaphoreGive(h->lock);
    }
}

esp_err_t drv_drum_create(drv_drum_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "out_handle nulo");
    *out_handle = NULL;

    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DRUM_IN1) | (1ULL << PIN_DRUM_ENA),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_set_level(PIN_DRUM_IN1, 0);
    gpio_set_level(PIN_DRUM_ENA, 0);
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config fallo");
    gpio_set_level(PIN_DRUM_IN1, 0);
    gpio_set_level(PIN_DRUM_ENA, 0);

    const ledc_timer_config_t tcfg = {
        .speed_mode      = DRUM_LEDC_MODE,
        .duty_resolution = DRUM_LEDC_BITS,
        .timer_num       = DRUM_LEDC_TIMER,
        .freq_hz         = CONFIG_DRUM_PWM_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc_timer_config fallo");
    const ledc_channel_config_t ccfg = {
        .gpio_num   = PIN_DRUM_ENA,
        .speed_mode = DRUM_LEDC_MODE,
        .channel    = DRUM_LEDC_CH,
        .timer_sel  = DRUM_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc_channel_config fallo");

    drv_drum_handle_t h = calloc(1, sizeof(struct drv_drum_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->lock = xSemaphoreCreateMutex();
    if (!h->lock) {
        free(h);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(drum_task, "drum", DRUM_TASK_STACK, h,
                                                  DRUM_TASK_PRIO, NULL, DRUM_TASK_CORE);
    if (ok != pdPASS) {
        vSemaphoreDelete(h->lock);
        free(h);
        ESP_LOGE(TAG, "no se pudo crear la tarea");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Tambor listo, PARADO (L298N: ENA=GPIO%d PWM %d Hz, IN1=GPIO%d; pulso %d%% x %d ms, trabajo %d%%)",
             PIN_DRUM_ENA, CONFIG_DRUM_PWM_HZ, PIN_DRUM_IN1,
             CONFIG_DRUM_KICK_PCT, CONFIG_DRUM_KICK_MS, CONFIG_DRUM_SPEED_PCT);
    *out_handle = h;
    return ESP_OK;
}

void drv_drum_set(drv_drum_handle_t h, bool on)
{
    if (!h) {
        return;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (on && !h->commanded) {
        h->commanded = true;
        h->on_tick = xTaskGetTickCount();
        h->on_count++;
        apply_locked(h, profile_power(0));
    } else if (!on && h->commanded) {
        h->commanded = false;
        apply_locked(h, 0);
    }
    xSemaphoreGive(h->lock);
}

bool drv_drum_commanded(drv_drum_handle_t h)
{
    bool c = false;
    if (h) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        c = h->commanded;
        xSemaphoreGive(h->lock);
    }
    return c;
}

bool drv_drum_running(drv_drum_handle_t h)
{
    bool r = false;
    if (h) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        r = h->commanded && h->power > 0;
        xSemaphoreGive(h->lock);
    }
    return r;
}

uint8_t drv_drum_power(drv_drum_handle_t h)
{
    uint8_t p = 0;
    if (h) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        p = h->power;
        xSemaphoreGive(h->lock);
    }
    return p;
}

uint32_t drv_drum_cycles(drv_drum_handle_t h)
{
    uint32_t n = 0;
    if (h) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        n = h->on_count;
        xSemaphoreGive(h->lock);
    }
    return n;
}
