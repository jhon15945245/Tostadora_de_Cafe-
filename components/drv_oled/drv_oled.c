#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"

#include "drv_oled.h"
#include "oled_font5x7.h"

static const char *TAG = "drv_oled";

#define OLED_DEFAULT_ADDR  0x3C
#define OLED_PAGES         (DRV_OLED_H / 8)
#define OLED_I2C_TIMEOUT   100

struct drv_oled_s {
    i2c_master_dev_handle_t dev;
    uint8_t                 col_off;
    uint8_t                 fb[DRV_OLED_W * OLED_PAGES];
};

static esp_err_t send_cmds(drv_oled_handle_t h, const uint8_t *cmds, size_t n)
{
    uint8_t buf[32];
    ESP_RETURN_ON_FALSE(n < sizeof(buf), ESP_ERR_INVALID_SIZE, TAG, "comando muy largo");
    buf[0] = 0x00;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(h->dev, buf, n + 1, OLED_I2C_TIMEOUT);
}

static inline void put_px(drv_oled_handle_t h, int x, int y, bool on)
{
    if ((unsigned)x >= DRV_OLED_W || (unsigned)y >= DRV_OLED_H) {
        return;
    }
    uint8_t *b = &h->fb[(y >> 3) * DRV_OLED_W + x];
    if (on) {
        *b |= (uint8_t)(1u << (y & 7));
    } else {
        *b &= (uint8_t)~(1u << (y & 7));
    }
}

esp_err_t drv_oled_create(const drv_oled_config_t *cfg, drv_oled_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->bus && out_handle, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    *out_handle = NULL;

    drv_oled_handle_t h = calloc(1, sizeof(struct drv_oled_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->col_off = cfg->col_offset;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->addr ? cfg->addr : OLED_DEFAULT_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &h->dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo anadir el OLED al bus I2C (%s)", esp_err_to_name(ret));
        free(h);
        return ret;
    }

    static const uint8_t init_seq[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x02,
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0xCF,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0xAF,
    };
    ret = send_cmds(h, init_seq, sizeof(init_seq));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "el OLED no responde en 0x%02X (%s). Revisa SDA/SCL, la alimentacion y la direccion.",
                 cfg->addr ? cfg->addr : OLED_DEFAULT_ADDR, esp_err_to_name(ret));
        i2c_master_bus_rm_device(h->dev);
        free(h);
        return ret;
    }

    drv_oled_clear(h);
    drv_oled_flush(h);
    ESP_LOGI(TAG, "OLED listo (128x64, I2C 0x%02X)", cfg->addr ? cfg->addr : OLED_DEFAULT_ADDR);
    *out_handle = h;
    return ESP_OK;
}

void drv_oled_clear(drv_oled_handle_t h)
{
    if (h) {
        memset(h->fb, 0, sizeof(h->fb));
    }
}

void drv_oled_fill_rect(drv_oled_handle_t h, int x, int y, int w, int hgt, bool on)
{
    if (!h) {
        return;
    }
    for (int j = 0; j < hgt; j++) {
        for (int i = 0; i < w; i++) {
            put_px(h, x + i, y + j, on);
        }
    }
}

void drv_oled_rect(drv_oled_handle_t h, int x, int y, int w, int hgt)
{
    if (!h || w < 2 || hgt < 2) {
        return;
    }
    for (int i = 0; i < w; i++) {
        put_px(h, x + i, y, true);
        put_px(h, x + i, y + hgt - 1, true);
    }
    for (int j = 0; j < hgt; j++) {
        put_px(h, x, y + j, true);
        put_px(h, x + w - 1, y + j, true);
    }
}

void drv_oled_text(drv_oled_handle_t h, int x, int y, const char *s, uint8_t scale, bool inverted)
{
    if (!h || !s) {
        return;
    }
    if (scale == 0) {
        scale = 1;
    }
    const int len = (int)strlen(s);
    if (inverted) {
        drv_oled_fill_rect(h, x, y, len * 6 * scale, 8 * scale, true);
    }
    const bool ink = !inverted;

    for (int n = 0; n < len; n++) {
        unsigned char c = (unsigned char)s[n];
        if (c < 32 || c > 127) {
            c = '?';
        }
        const uint8_t *glyph = &OLED_FONT_5X7[(c - 32) * 5];
        const int gx = x + n * 6 * scale;
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 8; row++) {
                if (glyph[col] & (1u << row)) {
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            put_px(h, gx + col * scale + dx, y + row * scale + dy, ink);
                        }
                    }
                }
            }
        }
    }
}

esp_err_t drv_oled_flush(drv_oled_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "handle nulo");
    uint8_t data[1 + DRV_OLED_W];
    data[0] = 0x40;

    for (int page = 0; page < OLED_PAGES; page++) {
        const uint8_t cmd[3] = {
            (uint8_t)(0xB0 | page),
            (uint8_t)(h->col_off & 0x0F),
            (uint8_t)(0x10 | (h->col_off >> 4)),
        };
        ESP_RETURN_ON_ERROR(send_cmds(h, cmd, sizeof(cmd)), TAG, "fallo al elegir pagina");
        memcpy(&data[1], &h->fb[page * DRV_OLED_W], DRV_OLED_W);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(h->dev, data, sizeof(data), OLED_I2C_TIMEOUT),
                            TAG, "fallo al enviar datos");
    }
    return ESP_OK;
}
