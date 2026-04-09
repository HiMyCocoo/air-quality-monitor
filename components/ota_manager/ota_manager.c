#include "ota_manager_internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = OTA_MANAGER_TAG;

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_UPLOAD,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_READY,
    OTA_STATE_SUCCESS,
    OTA_STATE_ERROR,
} ota_state_t;

typedef enum {
    OTA_SOURCE_NONE = 0,
    OTA_SOURCE_GITHUB,
    OTA_SOURCE_UPLOAD,
} ota_source_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    ota_state_t state;
    ota_source_t source;
    bool enabled;
    bool busy;
    bool update_available;
    bool restart_pending;
    int progress_percent;
    int64_t last_checked_at_ms;
    int64_t last_updated_at_ms;
    size_t bytes_read;
    size_t total_bytes;
    char latest_version[FIRMWARE_VERSION_LEN];
    char release_tag[OTA_GITHUB_TAG_LEN];
    char asset_name[OTA_GITHUB_ASSET_NAME_LEN];
    char asset_url[OTA_GITHUB_URL_LEN];
    char release_url[OTA_GITHUB_URL_LEN];
    char status_text[OTA_STATUS_TEXT_LEN];
    char last_error[OTA_ERROR_TEXT_LEN];
    ota_manager_callbacks_t callbacks;
} ota_manager_ctx_t;

static ota_manager_ctx_t s_ctx;

static bool ota_busy_locked(void);

static bool ota_lock_take(TickType_t timeout_ticks)
{
    if (s_ctx.lock == NULL) {
        return false;
    }
    return xSemaphoreTake(s_ctx.lock, timeout_ticks) == pdTRUE;
}

static void ota_lock_give(void)
{
    if (s_ctx.lock != NULL) {
        xSemaphoreGive(s_ctx.lock);
    }
}

static void ota_format_text(char *buffer, size_t buffer_len, const char *fmt, va_list args)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    if (fmt == NULL || *fmt == '\0') {
        buffer[0] = '\0';
        return;
    }

    vsnprintf(buffer, buffer_len, fmt, args);
}

static void ota_set_status_text_locked(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ota_format_text(s_ctx.status_text, sizeof(s_ctx.status_text), fmt, args);
    va_end(args);
}

static void ota_set_error_text_locked(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ota_format_text(s_ctx.last_error, sizeof(s_ctx.last_error), fmt, args);
    va_end(args);
}

static const char *ota_state_key(ota_state_t state)
{
    switch (state) {
    case OTA_STATE_CHECKING:
        return "checking";
    case OTA_STATE_UPLOAD:
        return "uploading";
    case OTA_STATE_DOWNLOADING:
        return "downloading";
    case OTA_STATE_READY:
        return "ready";
    case OTA_STATE_SUCCESS:
        return "success";
    case OTA_STATE_ERROR:
        return "error";
    case OTA_STATE_IDLE:
    default:
        return "idle";
    }
}

static const char *ota_source_key(ota_source_t source)
{
    switch (source) {
    case OTA_SOURCE_GITHUB:
        return "github";
    case OTA_SOURCE_UPLOAD:
        return "upload";
    case OTA_SOURCE_NONE:
    default:
        return "none";
    }
}

static void ota_clear_release_info_locked(void)
{
    s_ctx.update_available = false;
    s_ctx.latest_version[0] = '\0';
    s_ctx.release_tag[0] = '\0';
    s_ctx.asset_name[0] = '\0';
    s_ctx.asset_url[0] = '\0';
    s_ctx.release_url[0] = '\0';
}

static void ota_store_release_info_locked(const ota_release_info_t *info, bool update_available)
{
    ota_clear_release_info_locked();
    if (info == NULL) {
        return;
    }

    s_ctx.update_available = update_available;
    strlcpy(s_ctx.latest_version, info->version, sizeof(s_ctx.latest_version));
    strlcpy(s_ctx.release_tag, info->tag, sizeof(s_ctx.release_tag));
    strlcpy(s_ctx.asset_name, info->asset_name, sizeof(s_ctx.asset_name));
    strlcpy(s_ctx.asset_url, info->asset_url, sizeof(s_ctx.asset_url));
    strlcpy(s_ctx.release_url, info->release_url, sizeof(s_ctx.release_url));
}

static void ota_prepare_locked(ota_state_t state, ota_source_t source, const char *status_text)
{
    s_ctx.state = state;
    s_ctx.source = source;
    s_ctx.busy = true;
    s_ctx.restart_pending = false;
    s_ctx.progress_percent = 0;
    s_ctx.bytes_read = 0;
    s_ctx.total_bytes = 0;
    s_ctx.last_updated_at_ms = 0;
    s_ctx.last_error[0] = '\0';
    ota_set_status_text_locked("%s", status_text != NULL ? status_text : "");
}

static void ota_finish_ready_locked(const ota_release_info_t *info)
{
    ota_store_release_info_locked(info, true);
    s_ctx.busy = false;
    s_ctx.source = OTA_SOURCE_GITHUB;
    s_ctx.state = OTA_STATE_READY;
    s_ctx.progress_percent = 0;
    s_ctx.last_checked_at_ms = esp_timer_get_time() / 1000;
    ota_set_status_text_locked("检测到新版本 %s，可直接升级", s_ctx.latest_version);
}

static void ota_finish_up_to_date_locked(const ota_release_info_t *info)
{
    ota_store_release_info_locked(info, false);
    s_ctx.busy = false;
    s_ctx.source = OTA_SOURCE_GITHUB;
    s_ctx.state = OTA_STATE_IDLE;
    s_ctx.progress_percent = 0;
    s_ctx.last_checked_at_ms = esp_timer_get_time() / 1000;
    ota_set_status_text_locked("当前已经是 GitHub 最新版本");
}

static void ota_finish_success_locked(const char *status_text)
{
    s_ctx.busy = false;
    s_ctx.state = OTA_STATE_SUCCESS;
    s_ctx.restart_pending = true;
    s_ctx.progress_percent = 100;
    s_ctx.last_updated_at_ms = esp_timer_get_time() / 1000;
    s_ctx.last_error[0] = '\0';
    ota_set_status_text_locked("%s", status_text != NULL ? status_text : "固件升级完成，即将重启");
}

static void ota_finish_error_locked(const char *message)
{
    s_ctx.busy = false;
    s_ctx.state = OTA_STATE_ERROR;
    s_ctx.restart_pending = false;
    if (message != NULL && *message != '\0') {
        ota_set_error_text_locked("%s", message);
        ota_set_status_text_locked("%s", message);
    } else {
        ota_set_error_text_locked("固件升级失败");
        ota_set_status_text_locked("固件升级失败");
    }
}

static void ota_update_progress_locked(size_t bytes_read, size_t total_bytes, const char *status_text)
{
    s_ctx.bytes_read = bytes_read;
    s_ctx.total_bytes = total_bytes;
    if (total_bytes > 0 && bytes_read <= total_bytes) {
        s_ctx.progress_percent = (int)((bytes_read * 100U) / total_bytes);
    }
    if (status_text != NULL && *status_text != '\0') {
        ota_set_status_text_locked("%s", status_text);
    }
}

esp_err_t ota_manager_begin_upload(size_t total_bytes, char *error, size_t error_len)
{
    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        snprintf(error, error_len, "OTA 状态锁不可用");
        return ESP_ERR_TIMEOUT;
    }
    if (ota_busy_locked()) {
        ota_lock_give();
        snprintf(error, error_len, "OTA 升级正在进行中，请稍后再试");
        return ESP_ERR_INVALID_STATE;
    }

    ota_prepare_locked(OTA_STATE_UPLOAD, OTA_SOURCE_UPLOAD, "正在上传本地固件...");
    s_ctx.total_bytes = total_bytes;
    ota_lock_give();
    return ESP_OK;
}

void ota_manager_report_progress(size_t bytes_read, size_t total_bytes, const char *status_text)
{
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        ota_update_progress_locked(bytes_read, total_bytes, status_text);
        ota_lock_give();
    }
}

void ota_manager_report_error(const char *message)
{
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        ota_finish_error_locked(message);
        ota_lock_give();
    }
}

void ota_manager_report_success(const char *status_text)
{
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        ota_finish_success_locked(status_text);
        ota_lock_give();
    }
}

void ota_manager_request_restart_from_worker(void)
{
    if (s_ctx.callbacks.request_restart != NULL) {
        s_ctx.callbacks.request_restart(s_ctx.callbacks.user_ctx);
    }
}

static void ota_mark_task_finished(void)
{
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        s_ctx.task = NULL;
        ota_lock_give();
    }
}

static void github_ota_task(void *arg)
{
    (void)arg;

    char error[OTA_ERROR_TEXT_LEN] = {0};
    ota_release_info_t info = {0};
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_version = app_desc != NULL ? app_desc->version : "";

    esp_err_t err = ota_manager_github_fetch_latest_release(&info, error, sizeof(error));
    if (err != ESP_OK) {
        ota_manager_report_error(error);
        ota_mark_task_finished();
        vTaskDelete(NULL);
        return;
    }

    bool update_available = ota_manager_compare_versions(current_version, info.version) < 0;
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        if (update_available) {
            ota_store_release_info_locked(&info, true);
            ota_prepare_locked(OTA_STATE_DOWNLOADING, OTA_SOURCE_GITHUB, "正在从 GitHub 下载 OTA 固件...");
            strlcpy(s_ctx.latest_version, info.version, sizeof(s_ctx.latest_version));
            strlcpy(s_ctx.release_tag, info.tag, sizeof(s_ctx.release_tag));
            strlcpy(s_ctx.asset_name, info.asset_name, sizeof(s_ctx.asset_name));
            strlcpy(s_ctx.asset_url, info.asset_url, sizeof(s_ctx.asset_url));
            strlcpy(s_ctx.release_url, info.release_url, sizeof(s_ctx.release_url));
            s_ctx.last_checked_at_ms = esp_timer_get_time() / 1000;
        } else {
            ota_finish_up_to_date_locked(&info);
        }
        ota_lock_give();
    }

    if (!update_available) {
        ota_mark_task_finished();
        vTaskDelete(NULL);
        return;
    }

    err = ota_manager_github_download_and_apply_ota(&info, error, sizeof(error));
    if (err == ESP_OK) {
        ota_manager_report_success("GitHub OTA 下载完成，设备即将重启");
    } else {
        ota_manager_report_error(error);
    }

    ota_mark_task_finished();
    if (err == ESP_OK) {
        ota_manager_request_restart_from_worker();
    }
    vTaskDelete(NULL);
}

static bool ota_busy_locked(void)
{
    return s_ctx.busy || s_ctx.task != NULL || s_ctx.restart_pending;
}

esp_err_t ota_manager_start(const ota_manager_callbacks_t *callbacks)
{
    ota_manager_stop();
    if (s_ctx.lock != NULL) {
        bool ota_running = false;
        if (xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ota_running = s_ctx.task != NULL || s_ctx.busy;
            xSemaphoreGive(s_ctx.lock);
        }
        if (ota_running) {
            ESP_LOGW(TAG, "start skipped because ota task is still running");
            return ESP_ERR_INVALID_STATE;
        }
        vSemaphoreDelete(s_ctx.lock);
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    if (callbacks != NULL) {
        s_ctx.callbacks = *callbacks;
    }
    s_ctx.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "ota mutex alloc failed");
    s_ctx.enabled = ota_manager_github_is_configured();
    s_ctx.source = OTA_SOURCE_NONE;
    s_ctx.state = OTA_STATE_IDLE;
    ota_set_status_text_locked(s_ctx.enabled ? "等待检查 GitHub 最新版本" : "当前固件未启用 GitHub 自动更新");
    return ESP_OK;
}

void ota_manager_stop(void)
{
    bool ota_running = false;
    if (s_ctx.lock != NULL && xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ota_running = s_ctx.task != NULL || s_ctx.busy;
        xSemaphoreGive(s_ctx.lock);
    }
    if (!ota_running) {
        if (s_ctx.lock != NULL) {
            vSemaphoreDelete(s_ctx.lock);
        }
        memset(&s_ctx, 0, sizeof(s_ctx));
    }
}

esp_err_t ota_manager_append_status_json(cJSON *root)
{
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "ota root json null");

    cJSON *ota_json = cJSON_AddObjectToObject(root, "ota");
    ESP_RETURN_ON_FALSE(ota_json != NULL, ESP_ERR_NO_MEM, TAG, "ota json alloc failed");

    ota_manager_ctx_t snapshot = {0};
    snapshot.enabled = ota_manager_github_is_configured();
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        snapshot = s_ctx;
        ota_lock_give();
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddBoolToObject(ota_json, "enabled", snapshot.enabled);
    cJSON_AddBoolToObject(ota_json, "busy", snapshot.busy);
    cJSON_AddBoolToObject(ota_json, "update_available", snapshot.update_available);
    cJSON_AddBoolToObject(ota_json, "restart_pending", snapshot.restart_pending);
    cJSON_AddStringToObject(ota_json, "state", ota_state_key(snapshot.state));
    cJSON_AddStringToObject(ota_json, "source", ota_source_key(snapshot.source));
    cJSON_AddStringToObject(ota_json, "current_version", app_desc != NULL ? app_desc->version : "");
    cJSON_AddStringToObject(ota_json, "latest_version", snapshot.latest_version);
    cJSON_AddStringToObject(ota_json, "release_tag", snapshot.release_tag);
    cJSON_AddStringToObject(ota_json, "asset_name", snapshot.asset_name);
    cJSON_AddStringToObject(ota_json, "asset_url", snapshot.asset_url);
    cJSON_AddStringToObject(ota_json, "release_url", snapshot.release_url);
    cJSON_AddStringToObject(ota_json, "status_text", snapshot.status_text);
    cJSON_AddStringToObject(ota_json, "last_error", snapshot.last_error);
    cJSON_AddStringToObject(ota_json, "github_repo", ota_manager_github_repo_name());
    cJSON_AddNumberToObject(ota_json, "progress_percent", snapshot.progress_percent);
    cJSON_AddNumberToObject(ota_json, "bytes_read", (double)snapshot.bytes_read);
    cJSON_AddNumberToObject(ota_json, "total_bytes", (double)snapshot.total_bytes);
    if (snapshot.last_checked_at_ms > 0) {
        cJSON_AddNumberToObject(ota_json, "last_checked_at_ms", (double)snapshot.last_checked_at_ms);
    } else {
        cJSON_AddNullToObject(ota_json, "last_checked_at_ms");
    }
    if (snapshot.last_updated_at_ms > 0) {
        cJSON_AddNumberToObject(ota_json, "last_updated_at_ms", (double)snapshot.last_updated_at_ms);
    } else {
        cJSON_AddNullToObject(ota_json, "last_updated_at_ms");
    }

    return ESP_OK;
}

esp_err_t ota_manager_check_github_release(char *error, size_t error_len)
{
    if (!ota_manager_github_is_configured()) {
        snprintf(error, error_len, "当前固件未启用 GitHub 自动更新");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        snprintf(error, error_len, "OTA 状态锁不可用");
        return ESP_ERR_TIMEOUT;
    }
    if (ota_busy_locked()) {
        ota_lock_give();
        snprintf(error, error_len, "OTA 升级正在进行中，请稍后再试");
        return ESP_ERR_INVALID_STATE;
    }
    ota_prepare_locked(OTA_STATE_CHECKING, OTA_SOURCE_GITHUB, "正在查询 GitHub 最新版本...");
    ota_lock_give();

    ota_release_info_t info = {0};
    esp_err_t err = ota_manager_github_fetch_latest_release(&info, error, error_len);
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_version = app_desc != NULL ? app_desc->version : "";

    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        snprintf(error, error_len, "OTA 状态锁不可用");
        return ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        if (ota_manager_compare_versions(current_version, info.version) < 0) {
            ota_finish_ready_locked(&info);
        } else {
            ota_finish_up_to_date_locked(&info);
        }
    } else {
        ota_finish_error_locked(error);
    }
    ota_lock_give();

    return err;
}

esp_err_t ota_manager_start_github_update(char *error, size_t error_len)
{
    if (!ota_manager_github_is_configured()) {
        snprintf(error, error_len, "当前固件未启用 GitHub 自动更新");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        snprintf(error, error_len, "OTA 状态锁不可用");
        return ESP_ERR_TIMEOUT;
    }
    if (ota_busy_locked()) {
        ota_lock_give();
        snprintf(error, error_len, "OTA 升级正在进行中，请稍后再试");
        return ESP_ERR_INVALID_STATE;
    }
    ota_prepare_locked(OTA_STATE_CHECKING, OTA_SOURCE_GITHUB, "正在查询 GitHub 最新版本...");
    ota_lock_give();

    TaskHandle_t task = NULL;
    if (xTaskCreate(github_ota_task,
                    "github_ota_task",
                    OTA_DOWNLOAD_TASK_STACK_SIZE,
                    NULL,
                    OTA_DOWNLOAD_TASK_PRIORITY,
                    &task) != pdPASS) {
        ota_manager_report_error("无法启动 GitHub OTA 后台任务");
        snprintf(error, error_len, "无法启动 GitHub OTA 后台任务");
        return ESP_ERR_NO_MEM;
    }

    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        snprintf(error, error_len, "OTA 状态锁不可用");
        return ESP_ERR_TIMEOUT;
    }
    s_ctx.task = task;
    ota_lock_give();
    return ESP_OK;
}
