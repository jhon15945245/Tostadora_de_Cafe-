#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"

#include "drv_ds3231.h"

static const char *TAG = "drv_ds3231";

#define DS3231_ADDR     0x68
#define REG_TIME        0x00
#define REG_STATUS      0x0F
#define STATUS_OSF      0x80
#define I2C_TIMEOUT_MS  100

struct drv_ds3231_s {
    i2c_master_dev_handle_t dev;
};

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static uint8_t weekday(uint16_t y, uint8_t m, uint8_t d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) {
        y -= 1;
    }
    return (uint8_t)(((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7) + 1);
}

static esp_err_t read_regs(drv_ds3231_handle_t h, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(h->dev, &reg, 1, buf, n, I2C_TIMEOUT_MS);
}

esp_err_t drv_ds3231_create(i2c_master_bus_handle_t bus, drv_ds3231_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(bus && out_handle, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    *out_handle = NULL;

    drv_ds3231_handle_t h = calloc(1, sizeof(struct drv_ds3231_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS3231_ADDR,
        .scl_speed_hz    = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (ret != ESP_OK) {
        free(h);
        ESP_LOGE(TAG, "no se pudo anadir el DS3231 al bus I2C (%s)", esp_err_to_name(ret));
        return ret;
    }

    uint8_t status = 0;
    ret = read_regs(h, REG_STATUS, &status, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "el DS3231 no responde en 0x%02X (%s). Revisa SDA/SCL y la alimentacion.",
                 DS3231_ADDR, esp_err_to_name(ret));
        i2c_master_bus_rm_device(h->dev);
        free(h);
        return ret;
    }
    ESP_LOGI(TAG, "DS3231 listo (0x%02X)", DS3231_ADDR);
    *out_handle = h;
    return ESP_OK;
}

esp_err_t drv_ds3231_get(drv_ds3231_handle_t h, rtc_datetime_t *dt)
{
    ESP_RETURN_ON_FALSE(h && dt, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");

    uint8_t r[7];
    ESP_RETURN_ON_ERROR(read_regs(h, REG_TIME, r, sizeof(r)), TAG, "fallo la lectura I2C");

    dt->second = bcd2dec(r[0] & 0x7F);
    dt->minute = bcd2dec(r[1] & 0x7F);
    dt->hour   = bcd2dec(r[2] & 0x3F);
    dt->day    = bcd2dec(r[4] & 0x3F);
    dt->month  = bcd2dec(r[5] & 0x1F);
    dt->year   = (uint16_t)(2000 + bcd2dec(r[6]));

    if (dt->second > 59 || dt->minute > 59 || dt->hour > 23 ||
        dt->day < 1 || dt->day > 31 || dt->month < 1 || dt->month > 12) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t drv_ds3231_set(drv_ds3231_handle_t h, const rtc_datetime_t *dt)
{
    ESP_RETURN_ON_FALSE(h && dt, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    ESP_RETURN_ON_FALSE(dt->year >= 2000 && dt->year <= 2099 && dt->month >= 1 && dt->month <= 12 &&
                        dt->day >= 1 && dt->day <= 31 && dt->hour <= 23 && dt->minute <= 59 && dt->second <= 59,
                        ESP_ERR_INVALID_ARG, TAG, "fecha u hora fuera de rango");

    const uint8_t buf[8] = {
        REG_TIME,
        dec2bcd(dt->second),
        dec2bcd(dt->minute),
        dec2bcd(dt->hour),
        dec2bcd(weekday(dt->year, dt->month, dt->day)),
        dec2bcd(dt->day),
        dec2bcd(dt->month),
        dec2bcd((uint8_t)(dt->year % 100)),
    };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(h->dev, buf, sizeof(buf), I2C_TIMEOUT_MS), TAG, "fallo la escritura I2C");

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_regs(h, REG_STATUS, &status, 1), TAG, "fallo al leer el estado");
    const uint8_t clr[2] = { REG_STATUS, (uint8_t)(status & ~STATUS_OSF) };
    return i2c_master_transmit(h->dev, clr, sizeof(clr), I2C_TIMEOUT_MS);
}

esp_err_t drv_ds3231_lost_power(drv_ds3231_handle_t h, bool *lost)
{
    ESP_RETURN_ON_FALSE(h && lost, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_regs(h, REG_STATUS, &status, 1), TAG, "fallo al leer el estado");
    *lost = (status & STATUS_OSF) != 0;
    return ESP_OK;
}
