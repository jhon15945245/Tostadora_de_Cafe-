idf_component_register(SRCS "drv_ds18b20.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES board_pins esp_rom)
