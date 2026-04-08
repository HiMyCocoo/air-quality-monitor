idf_component_register(SRCS "mqtt_ha.c"
                    INCLUDE_DIRS "include"
                    REQUIRES air_quality device_state_json platform mqtt cjson)
