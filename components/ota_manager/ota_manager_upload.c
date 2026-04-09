#include "ota_manager_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = OTA_MANAGER_TAG;

esp_err_t ota_manager_upload_firmware(httpd_req_t *req, char *error, size_t error_len)
{
    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "ota request null");

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        snprintf(error, error_len, "未找到 OTA 分区");
        return ESP_ERR_NOT_FOUND;
    }
    if (req->content_len == 0) {
        snprintf(error, error_len, "固件文件为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len > OTA_MAX_FIRMWARE_SIZE || req->content_len > partition->size) {
        snprintf(error, error_len, "固件大小无效或超过 OTA 分区限制");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ota_manager_begin_upload(req->content_len, error, error_len);
    if (err != ESP_OK) {
        return err;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t begin_err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (begin_err != ESP_OK) {
        ota_manager_report_error("OTA 分区初始化失败");
        snprintf(error, error_len, "OTA 分区初始化失败");
        return begin_err;
    }

    char chunk[1024];
    size_t remaining = req->content_len;
    size_t bytes_written = 0;
    while (remaining > 0) {
        size_t recv_len = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        int read = httpd_req_recv(req, chunk, recv_len);
        if (read <= 0) {
            esp_ota_abort(ota_handle);
            ota_manager_report_error("固件上传被中断，请重试");
            snprintf(error, error_len, "固件上传被中断，请重试");
            return ESP_FAIL;
        }

        esp_err_t write_err = esp_ota_write(ota_handle, chunk, read);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "ota write failed: %d", write_err);
            esp_ota_abort(ota_handle);
            ota_manager_report_error("固件写入失败，请重试");
            snprintf(error, error_len, "固件写入失败，请重试");
            return write_err;
        }

        remaining -= (size_t)read;
        bytes_written += (size_t)read;
        ota_manager_report_progress(bytes_written, req->content_len, "正在上传本地固件...");
    }

    esp_err_t end_err = esp_ota_end(ota_handle);
    if (end_err != ESP_OK) {
        ota_manager_report_error("固件校验失败，请确认上传的是 OTA 应用固件");
        snprintf(error, error_len, "固件校验失败，请确认上传的是 OTA 应用固件");
        return ESP_ERR_INVALID_ARG;
    }

    esp_app_desc_t new_desc = {0};
    esp_err_t desc_err = esp_ota_get_partition_description(partition, &new_desc);
    if (desc_err == ESP_OK) {
        const esp_app_desc_t *running_desc = esp_app_get_description();
        if (running_desc != NULL &&
            strncmp(new_desc.project_name, running_desc->project_name, sizeof(new_desc.project_name)) != 0) {
            ota_manager_report_error("固件项目名称不匹配，拒绝刷入非本项目固件");
            snprintf(error, error_len, "固件项目名称不匹配，拒绝刷入非本项目固件");
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t boot_err = esp_ota_set_boot_partition(partition);
    if (boot_err != ESP_OK) {
        ota_manager_report_error("切换启动分区失败");
        snprintf(error, error_len, "切换启动分区失败");
        return boot_err;
    }

    ota_manager_report_success("本地固件上传完成，设备即将重启");
    ota_manager_request_restart_from_worker();
    return ESP_OK;
}
