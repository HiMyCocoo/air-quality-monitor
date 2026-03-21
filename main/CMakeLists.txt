idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES platform sensors mqtt_ha provisioning_web esp_timer esp_event)
