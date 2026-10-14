idf_component_register(SRCS "svc_roast_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES roast_types svc_roast_profiles drv_heater drv_fan drv_drum
                       PRIV_REQUIRES esp_system esp_timer)
