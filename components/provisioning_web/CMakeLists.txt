idf_component_register(SRCS "provisioning_web.c"
                    INCLUDE_DIRS "include"
                    REQUIRES air_quality platform esp_http_server app_update json)
