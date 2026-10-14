idf_component_register(SRCS "drv_adkey.c"
                       INCLUDE_DIRS "include"
                       REQUIRES roast_types esp_adc
                       PRIV_REQUIRES board_pins)
