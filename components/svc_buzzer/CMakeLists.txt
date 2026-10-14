idf_component_register(SRCS "svc_buzzer.c"
                       INCLUDE_DIRS "include"
                       REQUIRES roast_types esp_driver_gpio
                       PRIV_REQUIRES board_pins)
