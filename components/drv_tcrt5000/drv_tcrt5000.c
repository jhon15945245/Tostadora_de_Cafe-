#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"

#include "board_pins.h"
#include "drv_tcrt5000.h"

static const char *TAG = "drv_tcrt5000";

struct drv_tcrt5000_s {
    adc_oneshot_unit_handle_t adc;
    adc_channel_t             channel;
};

esp_err_t drv_tcrt5000_create(adc_oneshot_unit_handle_t adc, drv_tcrt5000_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(adc && out_handle, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    *out_handle = NULL;

    drv_tcrt5000_handle_t h = calloc(1, sizeof(struct drv_tcrt5000_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->adc = adc;

    adc_unit_t unit;
    esp_err_t ret = adc_oneshot_io_to_channel(PIN_TCRT5000_AO, &unit, &h->channel);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "GPIO %d no es un pin ADC", PIN_TCRT5000_AO);
    ESP_GOTO_ON_FALSE(unit == ADC_UNIT_1, ESP_ERR_INVALID_ARG, err, TAG, "GPIO %d no es ADC1", PIN_TCRT5000_AO);

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(adc, h->channel, &chan_cfg);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "adc_oneshot_config_channel fallo");

    ESP_LOGI(TAG, "TCRT5000 listo (GPIO %d)", PIN_TCRT5000_AO);
    *out_handle = h;
    return ESP_OK;

err:
    free(h);
    return ret;
}

esp_err_t drv_tcrt5000_read(drv_tcrt5000_handle_t h, int *raw_out)
{
    ESP_RETURN_ON_FALSE(h && raw_out, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    int sum = 0;
    for (int i = 0; i < 4; i++) {
        int v = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(h->adc, h->channel, &v), TAG, "adc_oneshot_read fallo");
        sum += v;
    }
    *raw_out = sum / 4;
    return ESP_OK;
}
