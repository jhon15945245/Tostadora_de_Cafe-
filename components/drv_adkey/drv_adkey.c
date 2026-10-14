#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"

#include "board_pins.h"
#include "drv_adkey.h"

static const char *TAG = "drv_adkey";

#define ADKEY_DEFAULT_SAMPLES   8
#define ADKEY_DEFAULT_DEBOUNCE  3

typedef struct {
    input_key_t key;
    int         raw_center;
} adkey_level_t;

static const adkey_level_t LEVELS[] = {
    { INPUT_KEY_NONE,  CONFIG_ADKEY_RAW_IDLE  },
    { INPUT_KEY_UP,    CONFIG_ADKEY_RAW_UP    },
    { INPUT_KEY_DOWN,  CONFIG_ADKEY_RAW_DOWN  },
    { INPUT_KEY_LEFT,  CONFIG_ADKEY_RAW_LEFT  },
    { INPUT_KEY_RIGHT, CONFIG_ADKEY_RAW_RIGHT },
    { INPUT_KEY_OK,    CONFIG_ADKEY_RAW_OK    },
};
#define LEVEL_COUNT (sizeof(LEVELS) / sizeof(LEVELS[0]))

struct drv_adkey_s {
    adc_oneshot_unit_handle_t adc;
    bool                      owns_adc;
    adc_channel_t             channel;
    uint8_t                   samples;
    uint8_t                   debounce;
    int                       candidate;
    uint8_t                   count;
    input_key_t               stable;
};

static int classify(int raw)
{
    int best = -1;
    int best_dist = CONFIG_ADKEY_TOLERANCE + 1;

    for (size_t i = 0; i < LEVEL_COUNT; i++) {
        int dist = abs(raw - LEVELS[i].raw_center);
        if (dist < best_dist) {
            best_dist = dist;
            best = (int)LEVELS[i].key;
        }
    }
    return best;
}

esp_err_t drv_adkey_create(const drv_adkey_config_t *cfg, drv_adkey_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "out_handle nulo");
    *out_handle = NULL;

    esp_err_t ret = ESP_OK;
    drv_adkey_handle_t h = calloc(1, sizeof(struct drv_adkey_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");

    h->samples   = (cfg && cfg->samples)  ? cfg->samples  : ADKEY_DEFAULT_SAMPLES;
    h->debounce  = (cfg && cfg->debounce) ? cfg->debounce : ADKEY_DEFAULT_DEBOUNCE;
    h->candidate = -1;
    h->stable    = INPUT_KEY_NONE;

    adc_unit_t unit;
    ESP_GOTO_ON_ERROR(adc_oneshot_io_to_channel(PIN_ADKEY, &unit, &h->channel),
                      err, TAG, "GPIO %d no es un pin ADC", PIN_ADKEY);
    ESP_GOTO_ON_FALSE(unit == ADC_UNIT_1, ESP_ERR_INVALID_ARG, err, TAG,
                      "GPIO %d no es ADC1", PIN_ADKEY);

    if (cfg && cfg->adc) {
        h->adc = cfg->adc;
    } else {
        const adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
        ESP_GOTO_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &h->adc), err, TAG,
                          "fallo adc_oneshot_new_unit");
        h->owns_adc = true;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_GOTO_ON_ERROR(adc_oneshot_config_channel(h->adc, h->channel, &chan_cfg),
                      err, TAG, "fallo adc_oneshot_config_channel");

    ESP_LOGI(TAG, "Teclado listo (GPIO %d, %d muestras, antirrebote %d)",
             PIN_ADKEY, h->samples, h->debounce);
    *out_handle = h;
    return ESP_OK;

err:
    drv_adkey_destroy(h);
    return ret;
}

esp_err_t drv_adkey_poll(drv_adkey_handle_t h, input_key_t *key_out, int *raw_out)
{
    ESP_RETURN_ON_FALSE(h && key_out, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");

    int sum = 0;
    for (int i = 0; i < h->samples; i++) {
        int v = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(h->adc, h->channel, &v), TAG, "fallo adc_oneshot_read");
        sum += v;
    }
    const int raw = sum / h->samples;

    const int cand = classify(raw);
    if (cand < 0) {

        h->candidate = -1;
        h->count = 0;
    } else {
        if (cand == h->candidate) {
            if (h->count < 255) {
                h->count++;
            }
        } else {
            h->candidate = cand;
            h->count = 1;
        }
        if (h->count >= h->debounce) {
            h->stable = (input_key_t)cand;
        }
    }

    *key_out = h->stable;
    if (raw_out) {
        *raw_out = raw;
    }
    return ESP_OK;
}

void drv_adkey_destroy(drv_adkey_handle_t h)
{
    if (!h) {
        return;
    }
    if (h->owns_adc && h->adc) {
        adc_oneshot_del_unit(h->adc);
    }
    free(h);
}
