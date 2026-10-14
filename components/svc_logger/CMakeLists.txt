idf_component_register(SRCS "svc_logger.c"
                       INCLUDE_DIRS "include"
                       REQUIRES roast_types drv_ds3231 fatfs sdmmc esp_driver_sdspi esp_driver_spi
                       PRIV_REQUIRES board_pins)
