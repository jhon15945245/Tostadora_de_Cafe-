idf_component_register(SRCS "drv_drum.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_driver_ledc
                       PRIV_REQUIRES board_pins)
