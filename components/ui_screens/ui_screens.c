#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#include "svc_roast_profiles.h"
#include "ui_screens.h"

static const char *TAG = "ui_screens";

#define UI_TASK_STACK        4096
#define UI_TASK_PRIO         3
#define UI_TASK_CORE         0
#define UI_REDRAW_PERIOD_MS  200
#define UI_LIST_ROWS         6

typedef enum {
    SCR_SELECT = 0,
    SCR_CONFIRM,
    SCR_ROASTING,
    SCR_DONE,
    SCR_COUNT,
} screen_id_t;

typedef struct {
    void (*enter)(void);
    void (*on_input)(const input_event_t *ev);
    void (*on_status)(const roast_status_t *st);
    void (*draw)(const roast_status_t *st);
} screen_t;

struct ui_screens_s {
    ui_screens_config_t c;
    screen_id_t         current;
    roast_profile_id_t  selected;
    roast_state_t       last_state;
    TickType_t          enter_tick;
    bool                sd_ok;
};

static struct ui_screens_s *S;

static const screen_t SCREENS[SCR_COUNT];

static void goto_screen(screen_id_t id)
{
    S->current = id;
    S->enter_tick = xTaskGetTickCount();
    if (SCREENS[id].enter) {
        SCREENS[id].enter();
    }
}

static void send_cmd(roast_cmd_type_t type)
{
    const roast_cmd_t cmd = { .type = type, .profile = S->selected };
    if (xQueueSend(S->c.cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cola de comandos llena");
    }
}

static void line(int row, const char *s, bool inverted)
{
    if (inverted) {
        drv_oled_fill_rect(S->c.oled, 0, row * 8, DRV_OLED_W, 8, true);
    }
    drv_oled_text(S->c.oled, 0, row * 8, s, 1, inverted);
}

static const char *state_text(roast_state_t s)
{
    switch (s) {
    case ROAST_STATE_PREHEAT:  return "PRECALENTANDO";
    case ROAST_STATE_CHARGE:   return "CARGUE LOS GRANOS";
    case ROAST_STATE_ROASTING: return "TOSTANDO";
    case ROAST_STATE_COOLING:  return "ENFRIANDO";
    default:                   return "...";
    }
}

static const char *hint_text(roast_state_t s)
{
    switch (s) {
    case ROAST_STATE_PREHEAT:  return "OK largo: cancelar";
    case ROAST_STATE_CHARGE:   return "Cargue y pulse OK";
    case ROAST_STATE_ROASTING: return "OK largo: parar";
    case ROAST_STATE_COOLING:  return "Enfriando, espere";
    default:                   return "";
    }
}

static void select_enter(void) {}

static void select_on_input(const input_event_t *ev)
{
    const uint8_t count = svc_roast_profiles_count();
    if (ev->press != INPUT_PRESS_SHORT || count == 0) {
        return;
    }
    switch (ev->key) {
    case INPUT_KEY_UP:
        S->selected = (S->selected == 0) ? (uint8_t)(count - 1) : (uint8_t)(S->selected - 1);
        break;
    case INPUT_KEY_DOWN:
        S->selected = (uint8_t)((S->selected + 1) % count);
        break;
    case INPUT_KEY_OK:
        goto_screen(SCR_CONFIRM);
        break;
    default:
        break;
    }
}

static void select_draw(const roast_status_t *st)
{
    char buf[32];
    const uint8_t count = svc_roast_profiles_count();

    snprintf(buf, sizeof(buf), "%-15s%s", "TIPO DE TUESTE", S->sd_ok ? "SD:OK" : "SD:--");
    line(0, buf, false);

    const uint8_t top = (S->selected >= UI_LIST_ROWS) ? (uint8_t)(S->selected - (UI_LIST_ROWS - 1)) : 0;
    for (uint8_t i = 0; i < UI_LIST_ROWS; i++) {
        const uint8_t idx = (uint8_t)(top + i);
        if (idx >= count) {
            break;
        }
        const roast_profile_t *p = svc_roast_profiles_get(idx);
        snprintf(buf, sizeof(buf), " %-20.20s", p ? p->name : "-");
        line(1 + i, buf, idx == S->selected);
    }

#if CONFIG_BENCH_SIMULATE_SENSOR
    line(7, "** TEMP SIMULADA **", false);
#else
    if (!st->sensor_ok) {
        line(7, "SIN LECTURA TERMOCUPLA", false);
    } else {
        snprintf(buf, sizeof(buf), "ARR/ABA OK   %5.1fC", (double)st->temp_c);
        line(7, buf, false);
    }
#endif
}

static void confirm_enter(void) {}

static void confirm_on_input(const input_event_t *ev)
{
    if (ev->press != INPUT_PRESS_SHORT) {
        return;
    }
    if (ev->key == INPUT_KEY_OK) {
        send_cmd(ROAST_CMD_START);
        goto_screen(SCR_ROASTING);
    } else if (ev->key == INPUT_KEY_LEFT) {
        goto_screen(SCR_SELECT);
    }
}

static void confirm_draw(const roast_status_t *st)
{
    (void)st;
    char buf[32];
    const roast_profile_t *p = svc_roast_profiles_get(S->selected);
    if (!p) {
        return;
    }
    line(0, "CONFIRMAR TUESTE", false);
    drv_oled_text(S->c.oled, 0, 9, p->name, 1, false);

    for (uint8_t i = 0; i < p->stage_count; i++) {
        snprintf(buf, sizeof(buf), "%-10.10s %3.0fC %u:%02u", p->stages[i].name,
                 (double)p->stages[i].target_c,
                 (unsigned)(p->stages[i].duration_s / 60), (unsigned)(p->stages[i].duration_s % 60));
        drv_oled_text(S->c.oled, 0, 18 + 8 * i, buf, 1, false);
    }
    const unsigned total = svc_roast_profiles_total_s(p);
    snprintf(buf, sizeof(buf), "Total %u:%02u  Carga %.0fC", total / 60, total % 60, (double)p->charge_c);
    drv_oled_text(S->c.oled, 0, 44, buf, 1, false);
    drv_oled_text(S->c.oled, 0, 56, "OK:iniciar  <-:volver", 1, false);
}

static void roasting_enter(void)
{
    S->last_state = ROAST_STATE_IDLE;
}

static void roasting_on_input(const input_event_t *ev)
{
    if (ev->key != INPUT_KEY_OK) {
        return;
    }
    if (ev->press == INPUT_PRESS_SHORT && S->last_state == ROAST_STATE_CHARGE) {
        send_cmd(ROAST_CMD_CONFIRM_CHARGE);
    } else if (ev->press == INPUT_PRESS_LONG &&
               (S->last_state == ROAST_STATE_PREHEAT || S->last_state == ROAST_STATE_CHARGE ||
                S->last_state == ROAST_STATE_ROASTING || S->last_state == ROAST_STATE_FAULT)) {
        send_cmd(ROAST_CMD_STOP);
    }
}

static void roasting_on_status(const roast_status_t *st)
{
    S->last_state = st->state;
    if (st->state == ROAST_STATE_DONE) {
        goto_screen(SCR_DONE);
    } else if (st->state == ROAST_STATE_IDLE &&
               (xTaskGetTickCount() - S->enter_tick) > pdMS_TO_TICKS(1500)) {
        goto_screen(SCR_SELECT);
    }
}

static void draw_fault(const roast_status_t *st)
{
    char buf[32];
    line(0, "  FALLA DE SEGURIDAD", true);
    drv_oled_text(S->c.oled, 0, 14,
                  (st->fault == ROAST_FAULT_SENSOR) ? "Termocupla sin lectura"
                  : (st->fault == ROAST_FAULT_FAN)  ? "Ventilador sin giro"
                  : (st->fault == ROAST_FAULT_DRUM) ? "Tambor sin giro"
                                                    : "Temperatura maxima!",
                  1, false);
    snprintf(buf, sizeof(buf), "Temp: %.1f C", (double)st->temp_c);
    drv_oled_text(S->c.oled, 0, 28, buf, 1, false);
    drv_oled_text(S->c.oled, 0, 40, "Calentador: APAGADO", 1, false);
    drv_oled_text(S->c.oled, 0, 56, "OK largo: reiniciar", 1, false);
}

static void roasting_draw(const roast_status_t *st)
{
    if (st->state == ROAST_STATE_FAULT) {
        draw_fault(st);
        return;
    }
    char buf[32];
    const roast_profile_t *p = svc_roast_profiles_get(st->profile);
    const unsigned total = svc_roast_profiles_total_s(p);
    const unsigned el = (unsigned)st->elapsed_s;
    const bool heating = (st->state == ROAST_STATE_PREHEAT || st->state == ROAST_STATE_CHARGE ||
                          st->state == ROAST_STATE_ROASTING);

    snprintf(buf, sizeof(buf), "%-13.13s%8.8s", state_text(st->state), p ? p->name : "-");
    line(0, buf, false);

    if (st->sensor_ok) {
        snprintf(buf, sizeof(buf), "%.1f C", (double)st->temp_c);
        drv_oled_text(S->c.oled, 0, 10, buf, 3, false);
    } else {
        drv_oled_text(S->c.oled, 0, 14, "SIN SENSOR", 2, false);
    }

    const char drum_c = st->drum_on ? '*' : '-';
    const char fan_c = (st->fan_state == FAN_STATE_RUNNING) ? '*' : (st->fan_state == FAN_STATE_STARTING) ? '!' : '-';
    if (heating) {
        snprintf(buf, sizeof(buf), "Obj%4.0fC Q:%3u%% V%cT%c", (double)st->setpoint_c, (unsigned)st->heat_pct, fan_c, drum_c);
    } else {
        snprintf(buf, sizeof(buf), "Obj --  Q:%3u%% V%cT%c", (unsigned)st->heat_pct, fan_c, drum_c);
    }
    drv_oled_text(S->c.oled, 0, 34, buf, 1, false);

    snprintf(buf, sizeof(buf), "T %02u:%02u / %02u:%02u", el / 60, el % 60, total / 60, total % 60);
    drv_oled_text(S->c.oled, 0, 44, buf, 1, false);
    drv_oled_text(S->c.oled, 0, 54,
                  (heating && st->fan_state == FAN_STATE_STARTING) ? "Esperando ventilador" : hint_text(st->state),
                  1, false);

    int pct = 0;
    if (st->state == ROAST_STATE_PREHEAT) {
        pct = (p && p->charge_c > 0.0f) ? (int)(st->temp_c * 100.0f / p->charge_c) : 0;
    } else if (st->state == ROAST_STATE_CHARGE || st->state == ROAST_STATE_COOLING) {
        pct = 100;
    } else if (st->state == ROAST_STATE_ROASTING) {
        pct = total ? (int)(el * 100U / total) : 0;
    }
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    drv_oled_fill_rect(S->c.oled, 0, 62, pct * DRV_OLED_W / 100, 2, true);
}

static void done_enter(void) {}

static void done_on_input(const input_event_t *ev)
{
    if (ev->key == INPUT_KEY_OK && ev->press == INPUT_PRESS_SHORT) {
        send_cmd(ROAST_CMD_STOP);
    }
}

static void done_on_status(const roast_status_t *st)
{
    if (st->state == ROAST_STATE_IDLE) {
        goto_screen(SCR_SELECT);
    }
}

static void done_draw(const roast_status_t *st)
{
    char buf[32];
    const roast_profile_t *p = svc_roast_profiles_get(st->profile);
    const unsigned el = (unsigned)st->elapsed_s;

    drv_oled_text(S->c.oled, 0, 2, "TERMINADO", 2, false);
    snprintf(buf, sizeof(buf), "Perfil: %s", p ? p->name : "-");
    drv_oled_text(S->c.oled, 0, 24, buf, 1, false);
    snprintf(buf, sizeof(buf), "Tiempo: %02u:%02u", el / 60, el % 60);
    drv_oled_text(S->c.oled, 0, 34, buf, 1, false);
    snprintf(buf, sizeof(buf), "Temp:   %.1f C", (double)st->temp_c);
    drv_oled_text(S->c.oled, 0, 44, buf, 1, false);
    drv_oled_text(S->c.oled, 0, 56, "OK: nuevo tueste", 1, false);
}

static const screen_t SCREENS[SCR_COUNT] = {
    [SCR_SELECT]   = { select_enter,   select_on_input,   NULL,              select_draw },
    [SCR_CONFIRM]  = { confirm_enter,  confirm_on_input,  NULL,              confirm_draw },
    [SCR_ROASTING] = { roasting_enter, roasting_on_input, roasting_on_status, roasting_draw },
    [SCR_DONE]     = { done_enter,     done_on_input,     done_on_status,    done_draw },
};

static void redraw(const roast_status_t *st)
{
    drv_oled_clear(S->c.oled);
    SCREENS[S->current].draw(st);
    if (st->prox_alert && st->state != ROAST_STATE_FAULT) {

        drv_oled_fill_rect(S->c.oled, 0, 0, DRV_OLED_W, 8, true);
        drv_oled_text(S->c.oled, 0, 0, "! CUIDADO: CALIENTE !", 1, true);
    }
    drv_oled_flush(S->c.oled);
}

static void ui_task(void *arg)
{
    TickType_t last_draw = 0;
    (void)arg;

    goto_screen(SCR_SELECT);

    while (1) {
        input_event_t ev;
        const bool got = (xQueueReceive(S->c.input_queue, &ev, pdMS_TO_TICKS(50)) == pdTRUE);
        if (got) {
            ESP_LOGI(TAG, "input: tecla=%d tipo=%s", (int)ev.key, ev.press == INPUT_PRESS_LONG ? "larga" : "corta");
            if (ev.press == INPUT_PRESS_SHORT) {
                svc_buzzer_beep(S->c.buzzer);
            }
            if (SCREENS[S->current].on_input) {
                SCREENS[S->current].on_input(&ev);
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if (got || (now - last_draw) >= pdMS_TO_TICKS(UI_REDRAW_PERIOD_MS)) {
            roast_status_t st;
            if (xQueuePeek(S->c.status_queue, &st, 0) == pdTRUE) {
                S->sd_ok = (xEventGroupGetBits(S->c.events) & EVT_SD_OK) != 0;

                if (st.state == ROAST_STATE_FAULT && S->current != SCR_ROASTING) {
                    goto_screen(SCR_ROASTING);
                }
                if (SCREENS[S->current].on_status) {
                    SCREENS[S->current].on_status(&st);
                }
                redraw(&st);
            }
            last_draw = now;
        }
    }
}

esp_err_t ui_screens_create(const ui_screens_config_t *cfg, ui_screens_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->input_queue && cfg->cmd_queue && cfg->status_queue &&
                        cfg->oled && cfg->events && out_handle,
                        ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    ESP_RETURN_ON_FALSE(S == NULL, ESP_ERR_INVALID_STATE, TAG, "ya existe una instancia");
    *out_handle = NULL;

    struct ui_screens_s *h = calloc(1, sizeof(struct ui_screens_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->c = *cfg;
    S = h;

    const BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "ui_screens", UI_TASK_STACK, h,
                                                  UI_TASK_PRIO, NULL, UI_TASK_CORE);
    if (ok != pdPASS) {
        S = NULL;
        free(h);
        ESP_LOGE(TAG, "no se pudo crear la tarea");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Pantallas listas (OLED)");
    *out_handle = h;
    return ESP_OK;
}
