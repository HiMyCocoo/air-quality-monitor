idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_update esp_http_client esp_http_server esp_https_ota mbedtls cjson esp_timer platform)
