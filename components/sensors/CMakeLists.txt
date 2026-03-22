idf_component_register(SRCS "sensors.c" "scd41.c" "sgp41.c" "sps30.c" "sensirion_gas_index_algorithm.c"
                    INCLUDE_DIRS "include"
                    REQUIRES platform esp_driver_i2c esp_driver_uart esp_timer)
