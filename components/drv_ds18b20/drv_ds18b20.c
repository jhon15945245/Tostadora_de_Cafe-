#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_check.h"
#include "esp_log.h"

#include "board_pins.h"
#include "drv_ds18b20.h"

static const char *TAG = "drv_ds18b20";

#define OW_PIN   PIN_DS18B20

#define CMD_SKIP_ROM        0xCC
#define CMD_CONVERT_T       0x44
#define CMD_READ_SCRATCHPAD 0xBE

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

struct drv_ds18b20_s {
    const char *last_error;
    float       last_temp;
    bool        have_last;
};

static bool ow_reset(void)
{
    bool presence;

    gpio_set_level(OW_PIN, 0);
    esp_rom_delay_us(480);
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(OW_PIN, 1);
    esp_rom_delay_us(70);
    presence = (gpio_get_level(OW_PIN) == 0);
    portEXIT_CRITICAL(&s_mux);
    esp_rom_delay_us(410);
    return presence;
}

static void ow_write_bit(int bit)
{
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(OW_PIN, 0);
    esp_rom_delay_us(bit ? 6 : 60);
    gpio_set_level(OW_PIN, 1);
    esp_rom_delay_us(bit ? 64 : 10);
    portEXIT_CRITICAL(&s_mux);
}

static int ow_read_bit(void)
{
    int bit;
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(OW_PIN, 0);
    esp_rom_delay_us(6);
    gpio_set_level(OW_PIN, 1);
    esp_rom_delay_us(9);
    bit = gpio_get_level(OW_PIN);
    esp_rom_delay_us(55);
    portEXIT_CRITICAL(&s_mux);
    return bit;
}

static void ow_write_byte(uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(v & 1);
        v >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint8_t)(ow_read_bit() << i);
    }
    return v;
}

static uint8_t crc8(const uint8_t *d, size_t n)
{
    uint8_t crc = 0;
    while (n--) {
        uint8_t b = *d++;
        for (int i = 0; i < 8; i++) {
            const uint8_t mix = (uint8_t)((crc ^ b) & 1);
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            b >>= 1;
        }
    }
    return crc;
}

esp_err_t drv_ds18b20_create(drv_ds18b20_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "out_handle nulo");
    *out_handle = NULL;

    drv_ds18b20_handle_t h = calloc(1, sizeof(struct drv_ds18b20_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->last_error = "sin errores";

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << OW_PIN,
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        free(h);
        ESP_LOGE(TAG, "gpio_config fallo (%s)", esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(OW_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!ow_reset()) {
        ESP_LOGE(TAG, "el DS18B20 no responde en GPIO %d. Revisa DATA, VCC, GND y la resistencia de 4,7 kohm.", OW_PIN);
        free(h);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "DS18B20 listo (DATA en GPIO %d)", OW_PIN);
    *out_handle = h;
    return ESP_OK;
}

esp_err_t drv_ds18b20_start(drv_ds18b20_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "handle nulo");
    if (!ow_reset()) {
        h->last_error = "sin respuesta: DATA suelto, sin alimentacion o falta la resistencia de 4,7 kohm";
        return ESP_ERR_NOT_FOUND;
    }
    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_CONVERT_T);
    return ESP_OK;
}

esp_err_t drv_ds18b20_read(drv_ds18b20_handle_t h, float *temp_c)
{
    ESP_RETURN_ON_FALSE(h && temp_c, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");

    if (!ow_reset()) {
        h->last_error = "sin respuesta al leer el resultado";
        return ESP_ERR_NOT_FOUND;
    }
    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_READ_SCRATCHPAD);

    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_read_byte();
    }
    if (crc8(sp, 8) != sp[8]) {
        h->last_error = "datos corruptos (CRC): cable largo, ruido o falta la resistencia de 4,7 kohm";
        return ESP_ERR_INVALID_CRC;
    }

    const int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    const float t = (float)raw / 16.0f;

    if (raw == 0x0550 && !(h->have_last && h->last_temp > 75.0f)) {
        h->last_error = "valor de arranque (85 C) sin medir";
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (t < -55.0f || t > 125.0f) {
        h->last_error = "temperatura fuera del rango del sensor";
        return ESP_ERR_INVALID_RESPONSE;
    }

    h->last_temp = t;
    h->have_last = true;
    h->last_error = "sin errores";
    *temp_c = t;
    return ESP_OK;
}

const char *drv_ds18b20_last_error(drv_ds18b20_handle_t h)
{
    return h ? h->last_error : "handle nulo";
}
