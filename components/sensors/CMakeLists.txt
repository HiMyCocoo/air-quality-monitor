idf_component_register(SRCS "sensors.c" "scd41.c" "pms7003.c"
                    INCLUDE_DIRS "include"
                    REQUIRES platform esp_driver_i2c esp_driver_uart esp_driver_gpio esp_timer)
