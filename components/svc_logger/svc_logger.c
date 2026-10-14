#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_check.h"
#include "esp_log.h"

#include "board_pins.h"
#include "roast_types.h"
#include "svc_logger.h"

static const char *TAG = "svc_logger";

#define LOGGER_TASK_STACK  6144
#define LOGGER_TASK_PRIO   2
#define LOGGER_TASK_CORE   0
#define SD_HOST_ID         SPI3_HOST
#define MOUNT_POINT        "/sdcard"

#define CSV_HEADER "fecha_hora,t_s,temp_c,objetivo_c,calor_pct,estado,proximidad,ventilador_rpm,perfil"

struct svc_logger_s {
    svc_logger_config_t c;
    sdmmc_card_t       *card;
    bool                bus_ready;
    bool                mounted;
    FILE               *file;
    char                last_path[40];
};

static bool sd_mount(struct svc_logger_s *h)
{
    if (h->mounted) {
        return true;
    }

    if (!h->bus_ready) {
        const spi_bus_config_t bus_cfg = {
            .mosi_io_num     = PIN_SD_MOSI,
            .miso_io_num     = PIN_SD_MISO,
            .sclk_io_num     = PIN_SD_SCLK,
            .quadwp_io_num   = -1,
            .quadhd_io_num   = -1,
            .max_transfer_sz = 4000,
        };
        const esp_err_t ret = spi_bus_initialize(SD_HOST_ID, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "no se pudo iniciar el bus SPI de la SD (%s)", esp_err_to_name(ret));
            return false;
        }
        h->bus_ready = true;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 3,
        .allocation_unit_size   = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST_ID;
    host.max_freq_khz = 4000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs  = PIN_SD_CS;
    slot_cfg.host_id  = SD_HOST_ID;

    const esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &h->card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "no se pudo montar la microSD (%s). Se sigue sin registrar.", esp_err_to_name(ret));
        return false;
    }
    h->mounted = true;
    xEventGroupSetBits(h->c.events, EVT_SD_OK);
    ESP_LOGI(TAG, "microSD montada");
    return true;
}

static void close_file(struct svc_logger_s *h)
{
    if (h->file) {
        fclose(h->file);
        h->file = NULL;
        ESP_LOGI(TAG, "archivo cerrado");
    }
}

static void open_new_file(struct svc_logger_s *h)
{
    char path[40];
    struct stat st;
    bool named = false;

    rtc_datetime_t dt;
    if (h->c.rtc && drv_ds3231_get(h->c.rtc, &dt) == ESP_OK) {
        snprintf(path, sizeof(path), MOUNT_POINT "/%02u%02u%02u%02u.csv",
                 (unsigned)dt.month, (unsigned)dt.day, (unsigned)dt.hour, (unsigned)dt.minute);
        named = (stat(path, &st) != 0);
    }

    for (int n = 1; !named && n <= 9999; n++) {
        snprintf(path, sizeof(path), MOUNT_POINT "/T%04d.csv", n);
        named = (stat(path, &st) != 0);
    }

    h->file = fopen(path, "w");
    if (!h->file) {
        ESP_LOGE(TAG, "no se pudo crear %s", path);
        h->mounted = false;
        xEventGroupClearBits(h->c.events, EVT_SD_OK);
        return;
    }
    strlcpy(h->last_path, path, sizeof(h->last_path));
    fprintf(h->file, "%s\n", CSV_HEADER);
    fflush(h->file);
    printf("CSV,%s\n", CSV_HEADER);
    ESP_LOGI(TAG, "registrando en %s", path);
}

static void dump_sd(struct svc_logger_s *h)
{
    if (h->file) {
        printf("DUMP,hay un tueste en curso: detenlo primero (OK largo) y vuelve a pedir el volcado\n");
        return;
    }
    if (!sd_mount(h)) {
        printf("DUMP,la microSD no esta disponible\n");
        return;
    }

    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        printf("DUMP,no se pudo abrir la tarjeta\n");
        return;
    }
    char best[40] = "";
    int count = 0;
    printf("DUMP,archivos en la SD:\n");
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), MOUNT_POINT "/%s", de->d_name);
        const long size = (stat(path, &st) == 0) ? (long)st.st_size : -1L;
        printf("DUMP,  %s  (%ld bytes)\n", de->d_name, size);
        count++;
        const size_t n = strlen(de->d_name);
        if (n > 4 && strcasecmp(&de->d_name[n - 4], ".csv") == 0 && strcmp(path, best) > 0) {
            strlcpy(best, path, sizeof(best));
        }
    }
    closedir(dir);
    printf("DUMP,%d archivo(s)\n", count);

    const char *target = h->last_path[0] ? h->last_path : best;
    if (!target[0]) {
        printf("DUMP,no hay ningun .CSV en la tarjeta\n");
        return;
    }
    FILE *f = fopen(target, "r");
    if (!f) {
        printf("DUMP,no se pudo abrir %s\n", target);
        return;
    }
    printf("===== INICIO %s =====\n", target);
    char buf[160];
    int lines = 0;
    while (fgets(buf, sizeof(buf), f) && lines < 3000) {
        fputs(buf, stdout);
        lines++;
    }
    fclose(f);
    printf("===== FIN (%d lineas) =====\n", lines);
}

static void logger_task(void *arg)
{
    struct svc_logger_s *h = (struct svc_logger_s *)arg;

    sd_mount(h);

    while (1) {
        log_entry_t e;
        if (xQueueReceive(h->c.log_queue, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (e.evt) {
        case LOG_EVT_START:
            close_file(h);
            if (sd_mount(h)) {
                open_new_file(h);
            }
            break;

        case LOG_EVT_ROW: {
            char stamp[32] = "";
            rtc_datetime_t now;
            if (h->c.rtc && drv_ds3231_get(h->c.rtc, &now) == ESP_OK) {
                snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u",
                         (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
                         (unsigned)now.hour, (unsigned)now.minute, (unsigned)now.second);
            }
            char row[256];
            snprintf(row, sizeof(row), "%s,%lu,%.1f,%.1f,%u,%u,%u,%u,%s",
                     stamp, (unsigned long)e.t_s, (double)e.temp_c, (double)e.setpoint_c,
                     (unsigned)e.heat_pct, (unsigned)e.state, (unsigned)e.prox, (unsigned)e.fan_rpm, e.profile);

            printf("CSV,%s\n", row);

            if (h->file) {
                const int w = fprintf(h->file, "%s\n", row);
                fflush(h->file);
                fsync(fileno(h->file));
                if (w < 0) {
                    ESP_LOGE(TAG, "error de escritura en la SD");
                    close_file(h);
                    h->mounted = false;
                    xEventGroupClearBits(h->c.events, EVT_SD_OK);
                }
            }
            break;
        }

        case LOG_EVT_END:
            close_file(h);
            break;

        case LOG_EVT_DUMP:
            dump_sd(h);
            break;
        }
    }
}

esp_err_t svc_logger_create(const svc_logger_config_t *cfg, svc_logger_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->log_queue && cfg->events && out_handle,
                        ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    *out_handle = NULL;

    struct svc_logger_s *h = calloc(1, sizeof(struct svc_logger_s));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "sin memoria");
    h->c = *cfg;

    const BaseType_t ok = xTaskCreatePinnedToCore(logger_task, "logger", LOGGER_TASK_STACK, h,
                                                  LOGGER_TASK_PRIO, NULL, LOGGER_TASK_CORE);
    if (ok != pdPASS) {
        free(h);
        ESP_LOGE(TAG, "no se pudo crear la tarea");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Registro en SD listo");
    *out_handle = h;
    return ESP_OK;
}
