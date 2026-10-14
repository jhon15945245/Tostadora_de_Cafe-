idf_component_register(SRCS "main.c"
                       REQUIRES board_pins roast_types
                                drv_heater drv_fan drv_drum drv_max6675 drv_ds18b20 drv_tcrt5000 drv_oled drv_ds3231 drv_adkey
                                svc_sensor svc_buzzer svc_logger svc_roast_manager svc_roast_profiles
                                ui_input ui_screens
                                esp_driver_gpio esp_driver_i2c esp_adc esp_app_format)
