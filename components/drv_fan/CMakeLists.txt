idf_component_register(SRCS "drv_fan.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES board_pins esp_timer)
