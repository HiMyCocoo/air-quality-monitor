#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    void (*request_restart)(void *user_ctx);
    void *user_ctx;
} ota_manager_callbacks_t;

esp_err_t ota_manager_start(const ota_manager_callbacks_t *callbacks);
void ota_manager_stop(void);
esp_err_t ota_manager_append_status_json(cJSON *root);
esp_err_t ota_manager_check_github_release(char *error, size_t error_len);
esp_err_t ota_manager_start_github_update(char *error, size_t error_len);
esp_err_t ota_manager_upload_firmware(httpd_req_t *req, char *error, size_t error_len);
