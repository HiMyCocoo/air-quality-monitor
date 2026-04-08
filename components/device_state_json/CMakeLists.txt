idf_component_register(SRCS "device_state_json.c"
                    INCLUDE_DIRS "include"
                    REQUIRES air_quality platform cjson esp_timer)
