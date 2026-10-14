idf_component_register(SRCS "svc_sensor.c"
                       INCLUDE_DIRS "include"
                       REQUIRES roast_types drv_max6675 drv_ds18b20 drv_tcrt5000
                       PRIV_REQUIRES esp_timer)
