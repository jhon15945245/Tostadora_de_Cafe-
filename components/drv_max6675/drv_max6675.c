#include <stdlib.h>
#include <string.h>
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include "board_pins.h"
#include "drv_max6675.h"

static const char *TAG = "drv_max6675";

#define MAX_SPI_HOST  SPI2_HOST

struct drv_max6675_s {
    spi_device_handle_t dev;
    uint16_t            last_raw;
};

esp_err_t drv_max6675_create(drv_max6675_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "out_handle nulo");
    *out_handle = NULL;

    drv_max6675_handle_t h = calloc(1, sizeof(struct drv_max6675_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");

    const spi_bus_config_t buscfg = {
        .mosi_io_num     = -1,
        .miso_io_num     = PIN_MAX_SO,
        .sclk_io_num     = PIN_MAX_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4,
    };
    esp_err_t ret = spi_bus_initialize(MAX_SPI_HOST, &buscfg, SPI_DMA_DISABLED);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "spi_bus_initialize fallo");

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = PIN_MAX_CS,
        .queue_size     = 1,
    };
    ret = spi_bus_add_device(MAX_SPI_HOST, &devcfg, &h->dev);
    ESP_GOTO_ON_ERROR(ret, err_bus, TAG, "spi_bus_add_device fallo");

    ESP_LOGI(TAG, "MAX6675 listo (SCK=%d SO=%d CS=%d)", PIN_MAX_SCK, PIN_MAX_SO, PIN_MAX_CS);
    *out_handle = h;
    return ESP_OK;

err_bus:
    spi_bus_free(MAX_SPI_HOST);
err:
    free(h);
    return ret;
}

esp_err_t drv_max6675_read(drv_max6675_handle_t h, float *temp_c)
{
    ESP_RETURN_ON_FALSE(h && temp_c, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 16;
    t.flags  = SPI_TRANS_USE_RXDATA;
    ESP_RETURN_ON_ERROR(spi_device_transmit(h->dev, &t), TAG, "fallo la transaccion SPI");

    const uint16_t raw = (uint16_t)((t.rx_data[0] << 8) | t.rx_data[1]);
    h->last_raw = raw;

    if (raw == 0x0000 || raw == 0xFFFF) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (raw & 0x8000) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (raw & 0x0004) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *temp_c = (float)(raw >> 3) * 0.25f;
    return ESP_OK;
}

uint16_t drv_max6675_last_raw(drv_max6675_handle_t h)
{
    return h ? h->last_raw : 0;
}
