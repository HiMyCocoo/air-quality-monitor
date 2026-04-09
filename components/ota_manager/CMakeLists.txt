idf_component_register(SRCS "ota_manager.c" "ota_manager_github.c" "ota_manager_upload.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_update esp_http_client esp_http_server esp_https_ota mbedtls cjson esp_timer platform)
