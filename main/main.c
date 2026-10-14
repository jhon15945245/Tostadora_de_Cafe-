#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_app_desc.h"
#include "driver/i2c_master.h"

#include "board_pins.h"
#include "roast_types.h"
#include "drv_heater.h"
#include "drv_fan.h"
#include "drv_drum.h"
#include "drv_max6675.h"
#include "drv_ds18b20.h"
#include "drv_tcrt5000.h"
#include "drv_oled.h"
#include "drv_ds3231.h"
#include "drv_adkey.h"
#include "svc_sensor.h"
#include "svc_buzzer.h"
#include "svc_logger.h"
#include "svc_roast_manager.h"
#include "ui_input.h"
#include "ui_screens.h"

#define ENABLE_KEYPAD       1
#define ENABLE_SERIAL_KEYS  1
#define KEYPAD_CALIBRATION  0
#define RTC_SET_FROM_BUILD  0
#define I2C_SCAN            0
#define OLED_I2C_ADDR       0x3C
#define OLED_COL_OFFSET     0

static const char *TAG = "MAIN";

#define CHECK_OR_RETURN(expr, what)                                              \
    do {                                                                         \
        esp_err_t _e = (expr);                                                   \
        if (_e != ESP_OK) {                                                      \
            ESP_LOGE(TAG, "%s: %s", (what), esp_err_to_name(_e));                \
            return;                                                              \
        }                                                                        \
    } while (0)

static QueueHandle_t      s_input_q;
static QueueHandle_t      s_cmd_q;
static QueueHandle_t      s_status_q;
static QueueHandle_t      s_sample_q;
static QueueHandle_t      s_log_q;
static SemaphoreHandle_t  s_sample_sem;
static EventGroupHandle_t s_events;

static drv_ds3231_handle_t s_rtc;
static drv_heater_handle_t s_heater;
static drv_fan_handle_t    s_fan;
static drv_drum_handle_t   s_drum;

static void rtc_set_from_build(drv_ds3231_handle_t rtc)
{
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const esp_app_desc_t *d = esp_app_get_description();
    char mon[4] = { 0 };
    unsigned day = 0, year = 0, hh = 0, mm = 0, ss = 0;

    if (sscanf(d->date, "%3s %u %u", mon, &day, &year) != 3 ||
        sscanf(d->time, "%u:%u:%u", &hh, &mm, &ss) != 3) {
        ESP_LOGE(TAG, "no se pudo leer la fecha de compilacion");
        return;
    }
    const char *p = strstr(MONTHS, mon);
    if (!p) {
        ESP_LOGE(TAG, "mes de compilacion desconocido: %s", mon);
        return;
    }
    const rtc_datetime_t dt = {
        .year = (uint16_t)year, .month = (uint8_t)((p - MONTHS) / 3 + 1), .day = (uint8_t)day,
        .hour = (uint8_t)hh, .minute = (uint8_t)mm, .second = (uint8_t)ss,
    };
    if (drv_ds3231_set(rtc, &dt) == ESP_OK) {
        ESP_LOGI(TAG, "RTC ajustado con la hora de compilacion: %04u-%02u-%02u %02u:%02u:%02u",
                 year, (unsigned)dt.month, day, hh, mm, ss);
    } else {
        ESP_LOGE(TAG, "no se pudo ajustar el RTC");
    }
}

#if ENABLE_SERIAL_KEYS

static void serial_set_time(void)
{
    char buf[32];
    int n = 0;
    ESP_LOGI(TAG, "Escribe la fecha y hora asi: 2026-10-08 15:30:00  y pulsa Enter (30 s)");
    const TickType_t start = xTaskGetTickCount();
    while (n < (int)sizeof(buf) - 1 && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(30000)) {
        const int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            break;
        }
        buf[n++] = (char)ch;
    }
    buf[n] = '\0';

    unsigned y, mo, d, h, mi, sec;
    if (sscanf(buf, " %u-%u-%u %u:%u:%u", &y, &mo, &d, &h, &mi, &sec) != 6) {
        ESP_LOGW(TAG, "formato no valido: '%s'", buf);
        return;
    }
    const rtc_datetime_t dt = {
        .year = (uint16_t)y, .month = (uint8_t)mo, .day = (uint8_t)d,
        .hour = (uint8_t)h, .minute = (uint8_t)mi, .second = (uint8_t)sec,
    };
    if (drv_ds3231_set(s_rtc, &dt) == ESP_OK) {
        ESP_LOGI(TAG, "RTC ajustado: %04u-%02u-%02u %02u:%02u:%02u", y, mo, d, h, mi, sec);
    } else {
        ESP_LOGE(TAG, "no se pudo ajustar el RTC (fecha fuera de rango)");
    }
}

static void serial_keys_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Teclas por serial: w/s/a/d = mover, e = OK, q = OK largo, t = ajustar hora del RTC, f = ver archivos de la SD, v = probar el ventilador, m = probar el tambor");
    while (1) {
        const int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == 'v') {
            const roast_cmd_t fan_cmd = { .type = ROAST_CMD_SET_FAN, .value = 2 };
            xQueueSend(s_cmd_q, &fan_cmd, 0);
            continue;
        }
        if (c == 'm') {
            const roast_cmd_t drum_cmd = { .type = ROAST_CMD_SET_DRUM, .value = 2 };
            xQueueSend(s_cmd_q, &drum_cmd, 0);
            continue;
        }
        if (c == 'f') {
            const log_entry_t dump = { .evt = LOG_EVT_DUMP };
            xQueueSend(s_log_q, &dump, 0);
            continue;
        }
        if (c == 't' && s_rtc) {
            serial_set_time();
            continue;
        }
        input_event_t ev = { .key = INPUT_KEY_NONE, .press = INPUT_PRESS_SHORT };
        switch (c) {
        case 'w': ev.key = INPUT_KEY_UP;    break;
        case 's': ev.key = INPUT_KEY_DOWN;  break;
        case 'a': ev.key = INPUT_KEY_LEFT;  break;
        case 'd': ev.key = INPUT_KEY_RIGHT; break;
        case 'e': ev.key = INPUT_KEY_OK;    break;
        case 'q': ev.key = INPUT_KEY_OK; ev.press = INPUT_PRESS_LONG; break;
        default: break;
        }
        if (ev.key != INPUT_KEY_NONE) {
            xQueueSend(s_input_q, &ev, 0);
        }
    }
}
#endif

#if ENABLE_KEYPAD && KEYPAD_CALIBRATION

static void keypad_calib_task(void *arg)
{
    drv_adkey_handle_t keys = (drv_adkey_handle_t)arg;
    static const char *const NAMES[] = { "ninguna", "ARRIBA", "ABAJO", "IZQUIERDA", "DERECHA", "OK" };
    ESP_LOGW(TAG, "=== CALIBRACION DEL TECLADO: pulsa cada boton y anota 'raw' ===");
    while (1) {
        input_key_t key = INPUT_KEY_NONE;
        int raw = 0;
        if (drv_adkey_poll(keys, &key, &raw) == ESP_OK) {
            ESP_LOGI(TAG, "raw=%4d  tecla reconocida: %s", raw,
                     ((int)key >= 0 && (int)key <= 5) ? NAMES[(int)key] : "?");
        } else {
            ESP_LOGE(TAG, "fallo la lectura del ADC");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
#endif

static void heartbeat_task(void *arg)
{
    static const char *const NAMES[] = {
        "heater", "sensor", "roast_mgr", "ui_input", "ui_screens", "logger", "buzzer", "drum",
    };
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "calentador: %u encendidos desde el arranque", (unsigned)drv_heater_cycles(s_heater));
        if (s_fan) {
            ESP_LOGI(TAG, "ventilador: %u rpm, %u encendidos desde el arranque",
                     (unsigned)drv_fan_rpm(s_fan), (unsigned)drv_fan_cycles(s_fan));
        }
        if (s_drum) {
            ESP_LOGI(TAG, "tambor: %s, potencia %u%%, %u arranques desde el reinicio",
                     drv_drum_running(s_drum) ? "GIRANDO" : "parado",
                     (unsigned)drv_drum_power(s_drum), (unsigned)drv_drum_cycles(s_drum));
        }
        if (s_rtc) {
            rtc_datetime_t t;
            if (drv_ds3231_get(s_rtc, &t) == ESP_OK) {
                ESP_LOGI(TAG, "RTC: %04u-%02u-%02u %02u:%02u:%02u", (unsigned)t.year, (unsigned)t.month,
                         (unsigned)t.day, (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
            } else {
                ESP_LOGW(TAG, "RTC: sin lectura valida (hora sin ajustar)");
            }
        }
        for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
            TaskHandle_t t = xTaskGetHandle(NAMES[i]);
            if (t) {
                ESP_LOGI(TAG, "pila libre %-10s: %u bytes", NAMES[i], (unsigned)uxTaskGetStackHighWaterMark(t));
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tostadora - arranque");
#if CONFIG_BENCH_SIMULATE_SENSOR
    ESP_LOGW(TAG, "*** MODO BANCO DE PRUEBAS: temperatura SIMULADA y calentador DESHABILITADO ***");
    ESP_LOGW(TAG, "*** Desactivalo en menuconfig antes de usar la tostadora de verdad ***");
#endif

    drv_heater_handle_t heater = NULL;
    CHECK_OR_RETURN(drv_heater_create(&heater), "calentador");
    s_heater = heater;

    drv_fan_handle_t fan = NULL;
#if !CONFIG_FAN_OUT_NONE
    CHECK_OR_RETURN(drv_fan_create(&fan), "ventilador");
    s_fan = fan;
#endif

    drv_drum_handle_t drum = NULL;
#if !CONFIG_DRUM_OUT_NONE
    CHECK_OR_RETURN(drv_drum_create(&drum), "tambor");
    s_drum = drum;
#endif

    s_input_q    = xQueueCreate(8, sizeof(input_event_t));
    s_cmd_q      = xQueueCreate(8, sizeof(roast_cmd_t));
    s_status_q   = xQueueCreate(1, sizeof(roast_status_t));
    s_sample_q   = xQueueCreate(1, sizeof(sensor_sample_t));
    s_log_q      = xQueueCreate(16, sizeof(log_entry_t));
    s_sample_sem = xSemaphoreCreateBinary();
    s_events     = xEventGroupCreate();
    if (!s_input_q || !s_cmd_q || !s_status_q || !s_sample_q || !s_log_q || !s_sample_sem || !s_events) {
        ESP_LOGE(TAG, "sin memoria para los objetos de FreeRTOS");
        return;
    }

    adc_oneshot_unit_handle_t adc = NULL;
    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    CHECK_OR_RETURN(adc_oneshot_new_unit(&adc_cfg, &adc), "ADC1");

    i2c_master_bus_handle_t i2c_bus = NULL;
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    CHECK_OR_RETURN(i2c_new_master_bus(&i2c_cfg, &i2c_bus), "bus I2C");

#if I2C_SCAN

    ESP_LOGW(TAG, "=== ESCANEO I2C (SDA=%d SCL=%d) ===", PIN_I2C_SDA, PIN_I2C_SCL);
    int found = 0;
    for (uint16_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(i2c_bus, addr, 50) == ESP_OK) {
            ESP_LOGW(TAG, "  responde el dispositivo 0x%02X", (unsigned)addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGE(TAG, "  NINGUN dispositivo responde: revisa SDA, SCL, VCC y GND");
    }
#endif

    drv_max6675_handle_t  thermocouple = NULL;
    drv_ds18b20_handle_t  ds18b20 = NULL;
    drv_tcrt5000_handle_t proximity = NULL;
    drv_oled_handle_t     oled = NULL;
#if CONFIG_TEMP_SENSOR_DS18B20
    ESP_LOGW(TAG, "*** SENSOR DS18B20 (solo pruebas, maximo 125 C): NO sirve para tostar ***");
    CHECK_OR_RETURN(drv_ds18b20_create(&ds18b20), "DS18B20");
#else
    CHECK_OR_RETURN(drv_max6675_create(&thermocouple), "MAX6675");
#endif
    CHECK_OR_RETURN(drv_tcrt5000_create(adc, &proximity), "TCRT5000");
    CHECK_OR_RETURN(drv_oled_create(&(drv_oled_config_t){ .bus = i2c_bus, .addr = OLED_I2C_ADDR,
                                                          .col_offset = OLED_COL_OFFSET }, &oled), "OLED");

    if (drv_ds3231_create(i2c_bus, &s_rtc) != ESP_OK) {
        ESP_LOGW(TAG, "RTC no encontrado: se registrara sin fecha ni hora");
        s_rtc = NULL;
    } else {
        bool lost = false;
        drv_ds3231_lost_power(s_rtc, &lost);
        if (lost || RTC_SET_FROM_BUILD) {
            if (lost) {
                ESP_LOGW(TAG, "El RTC perdio la hora (pila nueva o agotada): se ajusta con la hora de compilacion");
            } else {
                ESP_LOGW(TAG, "RTC_SET_FROM_BUILD activo: se ajusta con la hora de compilacion");
            }
            rtc_set_from_build(s_rtc);
        }
    }

    svc_buzzer_handle_t buzzer = NULL;
    CHECK_OR_RETURN(svc_buzzer_create(&(svc_buzzer_config_t){ .events = s_events }, &buzzer), "buzzer");

    svc_sensor_handle_t sensor = NULL;
    CHECK_OR_RETURN(svc_sensor_create(&(svc_sensor_config_t){
        .thermocouple = thermocouple, .ds18b20 = ds18b20, .proximity = proximity,
        .sample_queue = s_sample_q, .sample_sem = s_sample_sem, .events = s_events }, &sensor), "sensor");

    svc_roast_manager_handle_t manager = NULL;
    CHECK_OR_RETURN(svc_roast_manager_create(&(svc_roast_manager_config_t){
        .cmd_queue = s_cmd_q, .status_queue = s_status_q, .sample_queue = s_sample_q,
        .sample_sem = s_sample_sem, .log_queue = s_log_q, .events = s_events, .heater = heater, .fan = fan, .drum = drum }, &manager),
        "gestor del tueste");

    svc_logger_handle_t logger = NULL;
    CHECK_OR_RETURN(svc_logger_create(&(svc_logger_config_t){ .log_queue = s_log_q, .events = s_events, .rtc = s_rtc }, &logger),
                    "registro en SD");

#if ENABLE_KEYPAD
    drv_adkey_handle_t keys = NULL;
    CHECK_OR_RETURN(drv_adkey_create(&(drv_adkey_config_t){ .adc = adc }, &keys), "teclado");
#if KEYPAD_CALIBRATION
    xTaskCreatePinnedToCore(keypad_calib_task, "keypad_calib", 3072, keys, 2, NULL, 0);
#else
    ui_input_handle_t input = NULL;
    CHECK_OR_RETURN(ui_input_create(&(ui_input_config_t){ .keys = keys, .out_queue = s_input_q }, &input),
                    "entrada de usuario");
    (void)input;
#endif
#endif
    ui_screens_handle_t screens = NULL;
    CHECK_OR_RETURN(ui_screens_create(&(ui_screens_config_t){
        .input_queue = s_input_q, .cmd_queue = s_cmd_q, .status_queue = s_status_q,
        .oled = oled, .events = s_events, .buzzer = buzzer }, &screens), "pantallas");

#if ENABLE_SERIAL_KEYS
    xTaskCreatePinnedToCore(serial_keys_task, "serial_keys", 3072, NULL, 2, NULL, 0);
#endif
    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 3072, NULL, 1, NULL, 1);

    (void)sensor; (void)manager; (void)logger; (void)screens;
    ESP_LOGI(TAG, "Sistema en marcha");
}
