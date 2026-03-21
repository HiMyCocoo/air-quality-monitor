idf_component_register(SRCS "pms7003.c" "sensors.c" "scd41.c" "sps30.c"
                    INCLUDE_DIRS "include"
                    REQUIRES platform esp_driver_i2c esp_timer)
