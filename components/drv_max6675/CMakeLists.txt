idf_component_register(SRCS "drv_max6675.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_spi board_pins)
