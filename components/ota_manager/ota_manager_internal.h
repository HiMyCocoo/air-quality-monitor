#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "device_types.h"
#include "esp_err.h"
#include "ota_manager.h"

#define OTA_MAX_FIRMWARE_SIZE (2 * 1024 * 1024)
#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_GITHUB_TAG_LEN 32
#define OTA_GITHUB_ASSET_NAME_LEN 96
#define OTA_GITHUB_URL_LEN 512
#define OTA_GITHUB_AUTH_HEADER_LEN 512
#define OTA_STATUS_TEXT_LEN 128
#define OTA_ERROR_TEXT_LEN 192
#define OTA_DOWNLOAD_TASK_STACK_SIZE 12288
#define OTA_DOWNLOAD_TASK_PRIORITY 4
#define OTA_HTTP_USER_AGENT "air-quality-monitor-ota"
#define OTA_MANAGER_TAG "ota_manager"

typedef struct {
    char version[FIRMWARE_VERSION_LEN];
    char tag[OTA_GITHUB_TAG_LEN];
    char asset_name[OTA_GITHUB_ASSET_NAME_LEN];
    char asset_url[OTA_GITHUB_URL_LEN];
    char asset_api_url[OTA_GITHUB_URL_LEN];
    char release_url[OTA_GITHUB_URL_LEN];
} ota_release_info_t;

bool ota_manager_github_is_configured(void);
const char *ota_manager_github_repo_name(void);
int ota_manager_compare_versions(const char *left, const char *right);
esp_err_t ota_manager_github_fetch_latest_release(ota_release_info_t *info, char *error, size_t error_len);
esp_err_t ota_manager_github_download_and_apply_ota(const ota_release_info_t *info, char *error, size_t error_len);

esp_err_t ota_manager_begin_upload(size_t total_bytes, char *error, size_t error_len);
void ota_manager_report_progress(size_t bytes_read, size_t total_bytes, const char *status_text);
void ota_manager_report_error(const char *message);
void ota_manager_report_success(const char *status_text);
void ota_manager_request_restart_from_worker(void);
