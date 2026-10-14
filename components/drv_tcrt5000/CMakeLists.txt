idf_component_register(SRCS "drv_tcrt5000.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_adc
                       PRIV_REQUIRES board_pins)
