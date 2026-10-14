#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "svc_roast_profiles.h"
#include "svc_roast_manager.h"

static const char *TAG = "roast_mgr";

#define RM_WAIT_MS        100
#define RM_TASK_STACK     4096
#define RM_TASK_PRIO      4
#define RM_TASK_CORE      1

#if CONFIG_TEMP_SENSOR_DS18B20
#define MAX_TEMP_C        100.0f
#else
#define MAX_TEMP_C        ((float)CONFIG_SAFETY_MAX_TEMP_C)
#endif
#define COOL_DONE_C       ((float)CONFIG_SAFETY_COOL_DONE_C)
#define PREHEAT_TOL_C     2.0f
#define SENSOR_STALE_MS   1500

#define PID_KP            3.0f
#define PID_KI            0.08f
#define PID_I_MAX         50.0f

#define FAN_COOL_ON_C     ((float)CONFIG_FAN_COOL_ON_C)
#define FAN_COOL_OFF_C    ((float)CONFIG_FAN_COOL_OFF_C)

#define DRUM_FAULT_S      2.0f

#define FAN_FAULT_S       ((float)CONFIG_FAN_SPINUP_MS / 1000.0f + 3.0f)

#define IS_HEATING(s)  ((s) == ROAST_STATE_PREHEAT || (s) == ROAST_STATE_CHARGE || (s) == ROAST_STATE_ROASTING)
#define IS_ACTIVE(s)   (IS_HEATING(s) || (s) == ROAST_STATE_COOLING)

struct svc_roast_manager_s {
    svc_roast_manager_config_t c;

    roast_state_t          state;
    roast_fault_t          fault;
    roast_profile_id_t     profile;
    const roast_profile_t *prof;

    float    temp_c;
    float    setpoint_c;
    float    elapsed_s;
    float    run_s;
    float    integ;
    uint8_t  heat_pct;
    uint8_t  stage;
    bool     sensor_ok;
    bool     prox_alert;
    uint32_t last_log_s;
    float    sim_temp;

    bool     hot_hold;

    bool     drum_manual;
    float    drum_blocked_s;
    bool     drum_on;
    uint8_t  drum_pct;

    bool     fan_manual;
    float    fan_blocked_s;
    uint8_t  fan_state;
    uint16_t fan_rpm;
};

static const char *state_name(roast_state_t s)
{
    switch (s) {
    case ROAST_STATE_IDLE:     return "IDLE";
    case ROAST_STATE_PREHEAT:  return "PREHEAT";
    case ROAST_STATE_CHARGE:   return "CHARGE";
    case ROAST_STATE_ROASTING: return "ROASTING";
    case ROAST_STATE_MANUAL:   return "MANUAL";
    case ROAST_STATE_COOLING:  return "COOLING";
    case ROAST_STATE_DONE:     return "DONE";
    default:                   return "FAULT";
    }
}

static float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static void log_send(svc_roast_manager_handle_t h, log_evt_t evt)
{
    log_entry_t e;
    memset(&e, 0, sizeof(e));
    e.evt        = evt;
    e.t_s        = (uint32_t)h->run_s;
    e.temp_c     = h->temp_c;
    e.setpoint_c = h->setpoint_c;
    e.heat_pct   = h->heat_pct;
    e.state      = (uint8_t)h->state;
    e.prox       = h->prox_alert ? 1 : 0;
    e.fan_rpm    = h->fan_rpm;
    if (h->prof) {
        strlcpy(e.profile, h->prof->name, sizeof(e.profile));
    }
    if (xQueueSend(h->c.log_queue, &e, 0) != pdTRUE && evt != LOG_EVT_ROW) {
        ESP_LOGW(TAG, "cola de registro llena: se perdio un evento de control del archivo");
    }
}

static void enter_state(svc_roast_manager_handle_t h, roast_state_t next)
{
    ESP_LOGI(TAG, "estado: %s -> %s", state_name(h->state), state_name(next));
    if (next == ROAST_STATE_PREHEAT) {
        h->run_s = 0.0f;
        h->last_log_s = 0;
        h->state = next;
        log_send(h, LOG_EVT_START);
        return;
    }
    const bool was_active = IS_ACTIVE(h->state);
    h->state = next;
    if (was_active && !IS_ACTIVE(next)) {
        log_send(h, LOG_EVT_END);
    }
}

static uint32_t fault_event(roast_fault_t code)
{
    switch (code) {
    case ROAST_FAULT_SENSOR: return EVT_FAULT_SENSOR;
    case ROAST_FAULT_FAN:    return EVT_FAULT_FAN;
    case ROAST_FAULT_DRUM:   return EVT_FAULT_DRUM;
    default:                 return EVT_FAULT_OVERTEMP;
    }
}

static const char *fault_text(roast_fault_t code)
{
    switch (code) {
    case ROAST_FAULT_SENSOR: return "sensor sin lectura";
    case ROAST_FAULT_FAN:    return "ventilador sin giro";
    case ROAST_FAULT_DRUM:   return "tambor sin giro";
    default:                 return "sobretemperatura";
    }
}

static void enter_fault(svc_roast_manager_handle_t h, roast_fault_t code)
{
    drv_heater_off(h->c.heater);
    h->heat_pct = 0;
    h->integ = 0.0f;
    h->fault = code;
    xEventGroupSetBits(h->c.events, fault_event(code));
    ESP_LOGE(TAG, "FALLA DE SEGURIDAD: %s (temp %.1f C)", fault_text(code), (double)h->temp_c);
    enter_state(h, ROAST_STATE_FAULT);
}

static void publish(svc_roast_manager_handle_t h)
{
    const roast_status_t st = {
        .state      = h->state,
        .fault      = h->fault,
        .profile    = h->profile,
        .stage      = h->stage,
        .temp_c     = h->temp_c,
        .setpoint_c = h->setpoint_c,
        .heat_pct   = h->heat_pct,
        .fan_pct    = (h->fan_state != FAN_STATE_OFF) ? 100 : 0,
        .fan_state  = h->fan_state,
        .fan_rpm    = h->fan_rpm,
        .drum_on    = h->drum_on,
        .drum_pct   = h->drum_pct,
        .elapsed_s  = (uint32_t)h->elapsed_s,
        .sensor_ok  = h->sensor_ok,
        .prox_alert = h->prox_alert,
    };
    xQueueOverwrite(h->c.status_queue, &st);
}

static void handle_cmd(svc_roast_manager_handle_t h, const roast_cmd_t *cmd)
{
    switch (cmd->type) {
    case ROAST_CMD_START: {
        if (h->state != ROAST_STATE_IDLE) {
            ESP_LOGW(TAG, "START ignorado: no esta en IDLE");
            break;
        }
        if (!h->sensor_ok) {
            ESP_LOGW(TAG, "START ignorado: no hay lectura valida de la termocupla");
            break;
        }
        const roast_profile_t *p = svc_roast_profiles_get(cmd->profile);
        if (!p) {
            ESP_LOGW(TAG, "START ignorado: perfil %u no existe", (unsigned)cmd->profile);
            break;
        }
        h->fan_manual = false;
        h->drum_manual = false;
        h->prof = p;
        h->profile = cmd->profile;
        h->elapsed_s = 0.0f;
        h->stage = 0;
        h->integ = 0.0f;
        ESP_LOGI(TAG, "perfil elegido: %s", p->name);
        enter_state(h, ROAST_STATE_PREHEAT);
        break;
    }
    case ROAST_CMD_CONFIRM_CHARGE:
        if (h->state == ROAST_STATE_CHARGE) {
            enter_state(h, ROAST_STATE_ROASTING);
        }
        break;
    case ROAST_CMD_STOP:
        if (IS_HEATING(h->state)) {
            enter_state(h, ROAST_STATE_COOLING);
        } else if (h->state == ROAST_STATE_DONE) {
            enter_state(h, ROAST_STATE_IDLE);
        } else if (h->state == ROAST_STATE_FAULT) {
            if (h->sensor_ok && h->temp_c < (MAX_TEMP_C - 10.0f)) {
                xEventGroupClearBits(h->c.events, EVT_FAULT_ANY);
                h->fault = ROAST_FAULT_NONE;
                h->fan_blocked_s = 0.0f;
                h->drum_blocked_s = 0.0f;
                enter_state(h, ROAST_STATE_IDLE);
            } else {
                ESP_LOGW(TAG, "no se puede reiniciar: sensor sin lectura o temperatura aun alta");
            }
        }
        break;
    case ROAST_CMD_SET_FAN:
        if (h->state == ROAST_STATE_IDLE || h->state == ROAST_STATE_DONE) {
            h->fan_manual = (cmd->value == 2) ? !h->fan_manual : (cmd->value != 0);
            ESP_LOGI(TAG, "ventilador manual: %s", h->fan_manual ? "encendido" : "apagado");
        } else {
            ESP_LOGW(TAG, "el ventilador manual solo se usa en reposo");
        }
        break;
    case ROAST_CMD_SET_DRUM:
        if (h->state == ROAST_STATE_IDLE || h->state == ROAST_STATE_DONE) {
            h->drum_manual = (cmd->value == 2) ? !h->drum_manual : (cmd->value != 0);
            ESP_LOGI(TAG, "tambor manual: %s", h->drum_manual ? "girando" : "parado");
        } else {
            ESP_LOGW(TAG, "el tambor manual solo se usa en reposo");
        }
        break;
    case ROAST_CMD_MARK_FIRST_CRACK:
        ESP_LOGI(TAG, "primer crack marcado a los %u s", (unsigned)h->elapsed_s);
        break;
    default:
        ESP_LOGW(TAG, "comando no soportado aun: %d", (int)cmd->type);
        break;
    }
}

static void safety_check(svc_roast_manager_handle_t h, const sensor_sample_t *s)
{
    bool bad_sensor = true;
    if (s) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const bool stale = (uint32_t)(now_ms - s->t_ms) > SENSOR_STALE_MS;
        bad_sensor = stale || !s->sensor_ok;
        h->prox_alert = s->prox_alert;
        if (!bad_sensor) {
            h->temp_c = s->temp_c;
        }
    }
    h->sensor_ok = !bad_sensor;

    if (h->state == ROAST_STATE_FAULT) {
        return;
    }
    if (!bad_sensor && h->temp_c > MAX_TEMP_C) {
        enter_fault(h, ROAST_FAULT_OVERTEMP);
    } else if (bad_sensor && IS_ACTIVE(h->state)) {
        enter_fault(h, ROAST_FAULT_SENSOR);
    }
}

static float profile_setpoint(const roast_profile_t *p, float elapsed, uint8_t *stage_out)
{
    float t0 = 0.0f;
    float prev = p->charge_c;

    for (uint8_t i = 0; i < p->stage_count; i++) {
        const float dur = (float)p->stages[i].duration_s;
        if (elapsed < t0 + dur || i == p->stage_count - 1) {
            *stage_out = i;
            const float frac = (dur > 0.0f) ? clampf((elapsed - t0) / dur, 0.0f, 1.0f) : 1.0f;
            return prev + (p->stages[i].target_c - prev) * frac;
        }
        t0 += dur;
        prev = p->stages[i].target_c;
    }
    return prev;
}

static void control_step(svc_roast_manager_handle_t h, float dt)
{
    float sp = 0.0f;

    if (IS_ACTIVE(h->state)) {
        h->run_s += dt;
    }

    switch (h->state) {
    case ROAST_STATE_PREHEAT:
        sp = h->prof->charge_c;
        if (h->temp_c >= sp - PREHEAT_TOL_C) {
            enter_state(h, ROAST_STATE_CHARGE);
        }
        break;
    case ROAST_STATE_CHARGE:
        sp = h->prof->charge_c;
        break;
    case ROAST_STATE_ROASTING:
        h->elapsed_s += dt;
        sp = profile_setpoint(h->prof, h->elapsed_s, &h->stage);
        if (h->elapsed_s >= (float)svc_roast_profiles_total_s(h->prof)) {
            enter_state(h, ROAST_STATE_COOLING);
        }
        break;
    case ROAST_STATE_COOLING:
        if (h->temp_c <= COOL_DONE_C) {
            enter_state(h, ROAST_STATE_DONE);
        }
        break;
    default:
        break;
    }

    if (IS_HEATING(h->state)) {
        h->setpoint_c = sp;
        const float err = sp - h->temp_c;
        const float p_term = PID_KP * err;
        const float i_try = clampf(h->integ + PID_KI * err * dt, -PID_I_MAX, PID_I_MAX);
        const float out_try = p_term + i_try;
        if (out_try >= 0.0f && out_try <= 100.0f) {
            h->integ = i_try;
        }
        h->heat_pct = (uint8_t)clampf(p_term + h->integ, 0.0f, 100.0f);
    } else {
        h->setpoint_c = 0.0f;
        h->integ = 0.0f;
        h->heat_pct = 0;
    }

#if CONFIG_SAFETY_PROX_CUTS_HEATER
    if (h->prox_alert) {
        h->heat_pct = 0;
        h->integ = 0.0f;
    }
#endif
}

static void thermal_hold_step(svc_roast_manager_handle_t h)
{
    if (IS_ACTIVE(h->state) || h->state == ROAST_STATE_FAULT || h->state == ROAST_STATE_MANUAL) {
        h->hot_hold = true;
    } else if (h->sensor_ok) {
        if (h->temp_c >= FAN_COOL_ON_C) {
            h->hot_hold = true;
        } else if (h->temp_c <= FAN_COOL_OFF_C) {
            h->hot_hold = false;
        }
    }

}

static void fan_step(svc_roast_manager_handle_t h, float dt)
{
#if CONFIG_FAN_OUT_NONE
    (void)h;
    (void)dt;
#else
    drv_fan_handle_t fan = h->c.fan;
    if (!fan) {
        return;
    }

    const bool manual_ok = h->fan_manual && (h->state == ROAST_STATE_IDLE || h->state == ROAST_STATE_DONE);
    drv_fan_set(fan, h->hot_hold || manual_ok);

    h->fan_rpm = (uint16_t)drv_fan_rpm(fan);
    h->fan_state = !drv_fan_commanded(fan) ? FAN_STATE_OFF
                 : (drv_fan_spinning(fan) ? FAN_STATE_RUNNING : FAN_STATE_STARTING);

    if (IS_HEATING(h->state)) {
#if CONFIG_BENCH_SIMULATE_SENSOR
        const bool fan_ok = true;
#else
        const bool fan_ok = (h->fan_state == FAN_STATE_RUNNING);
#endif
        if (!fan_ok) {
            h->heat_pct = 0;
            h->integ = 0.0f;
            h->fan_blocked_s += dt;
            if (h->fan_blocked_s > FAN_FAULT_S) {
                enter_fault(h, ROAST_FAULT_FAN);
            }
        } else {
            h->fan_blocked_s = 0.0f;
        }
    } else {
        h->fan_blocked_s = 0.0f;
    }
#endif
}

static void drum_step(svc_roast_manager_handle_t h, float dt)
{
#if CONFIG_DRUM_OUT_NONE
    (void)h;
    (void)dt;
#else
    drv_drum_handle_t drum = h->c.drum;
    if (!drum) {
        return;
    }

    const bool manual_ok = h->drum_manual && (h->state == ROAST_STATE_IDLE || h->state == ROAST_STATE_DONE);
    drv_drum_set(drum, h->hot_hold || manual_ok);
    h->drum_on  = drv_drum_running(drum);
    h->drum_pct = drv_drum_power(drum);

    if (IS_HEATING(h->state)) {
#if CONFIG_BENCH_SIMULATE_SENSOR
        const bool drum_ok = true;
#else
        const bool drum_ok = h->drum_on;
#endif
        if (!drum_ok) {
            h->heat_pct = 0;
            h->integ = 0.0f;
            h->drum_blocked_s += dt;
            if (h->drum_blocked_s > DRUM_FAULT_S) {
                enter_fault(h, ROAST_FAULT_DRUM);
            }
        } else {
            h->drum_blocked_s = 0.0f;
        }
    } else {
        h->drum_blocked_s = 0.0f;
    }
#endif
}

static void manager_task(void *arg)
{
    svc_roast_manager_handle_t h = (svc_roast_manager_handle_t)arg;
    TickType_t last = xTaskGetTickCount();

    esp_task_wdt_add(NULL);
    publish(h);

    while (1) {

        xSemaphoreTake(h->c.sample_sem, pdMS_TO_TICKS(RM_WAIT_MS));
        esp_task_wdt_reset();

        roast_cmd_t cmd;
        while (xQueueReceive(h->c.cmd_queue, &cmd, 0) == pdTRUE) {
            handle_cmd(h, &cmd);
        }

        const TickType_t now = xTaskGetTickCount();
        float dt = (float)(now - last) * (float)portTICK_PERIOD_MS / 1000.0f;
        last = now;
        dt = clampf(dt, 0.0f, 1.0f);

        sensor_sample_t s;
        bool have = (xQueuePeek(h->c.sample_queue, &s, 0) == pdTRUE);
#if CONFIG_BENCH_SIMULATE_SENSOR

        h->sim_temp += (0.06f * (float)h->heat_pct - 0.02f * (h->sim_temp - 25.0f)) * dt;
        s.t_ms      = (uint32_t)(esp_timer_get_time() / 1000);
        s.temp_c    = h->sim_temp;
        s.sensor_ok = true;
        if (!have) {
            s.prox_raw = 4095;
            s.prox_alert = false;
        }
        have = true;
#endif
        safety_check(h, have ? &s : NULL);
        control_step(h, dt);
        thermal_hold_step(h);
        fan_step(h, dt);
        drum_step(h, dt);

        if (h->state == ROAST_STATE_FAULT) {
            drv_heater_off(h->c.heater);
        } else {
            drv_heater_set(h->c.heater, h->heat_pct);
        }

        if (IS_ACTIVE(h->state) && (uint32_t)h->run_s != h->last_log_s) {
            h->last_log_s = (uint32_t)h->run_s;
            log_send(h, LOG_EVT_ROW);
        }
        publish(h);
    }
}

esp_err_t svc_roast_manager_create(const svc_roast_manager_config_t *cfg,
                                   svc_roast_manager_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->cmd_queue && cfg->status_queue && cfg->sample_queue &&
                        cfg->sample_sem && cfg->log_queue && cfg->events && cfg->heater && out_handle,
                        ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    *out_handle = NULL;

    svc_roast_manager_handle_t h = calloc(1, sizeof(struct svc_roast_manager_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->c = *cfg;
    h->state = ROAST_STATE_IDLE;
    h->sim_temp = 25.0f;

    const BaseType_t ok = xTaskCreatePinnedToCore(manager_task, "roast_mgr", RM_TASK_STACK, h,
                                                  RM_TASK_PRIO, NULL, RM_TASK_CORE);
    if (ok != pdPASS) {
        free(h);
        ESP_LOGE(TAG, "no se pudo crear la tarea");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Gestor listo (limite %d C)", (int)MAX_TEMP_C);
    *out_handle = h;
    return ESP_OK;
}
