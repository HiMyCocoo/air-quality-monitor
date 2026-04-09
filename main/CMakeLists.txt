idf_component_register(SRCS "app_main.c" "led_strip_encoder.c" "status_led.c"
                    INCLUDE_DIRS "."
                    REQUIRES platform sensors air_quality mqtt_ha provisioning_web esp_timer esp_event esp_netif network_provisioning protocomm esp_driver_rmt)
