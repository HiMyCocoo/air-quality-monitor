#include "provisioning_web.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_quality.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "provisioning_web";

#define MQTT_PORT_MIN 1
#define MQTT_PORT_MAX 65535
#define MQTT_DEFAULT_PORT 1883
#define MQTT_URL_BUFFER_LEN 512
#define MQTT_SCHEME "mqtt://"
#define MAX_REQUEST_BODY_LEN 4096
#define OTA_MAX_FIRMWARE_SIZE (2 * 1024 * 1024)
#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_GITHUB_TAG_LEN 32
#define OTA_GITHUB_ASSET_NAME_LEN 96
#define OTA_GITHUB_URL_LEN 512
#define OTA_STATUS_TEXT_LEN 128
#define OTA_ERROR_TEXT_LEN 192
#define OTA_DOWNLOAD_TASK_STACK_SIZE 12288
#define OTA_DOWNLOAD_TASK_PRIORITY 4
#define OTA_HTTP_USER_AGENT "air-quality-monitor-ota"
#define SCD41_ALTITUDE_MIN 0
#define SCD41_ALTITUDE_MAX 3000
#define SCD41_TEMP_OFFSET_MIN 0.0
#define SCD41_TEMP_OFFSET_MAX 20.0

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_UPLOAD,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_READY,
    OTA_STATE_SUCCESS,
    OTA_STATE_ERROR,
} ota_state_t;

typedef struct {
    int major;
    int minor;
    int patch;
} version_triplet_t;

typedef struct {
    char version[FIRMWARE_VERSION_LEN];
    char tag[OTA_GITHUB_TAG_LEN];
    char asset_name[OTA_GITHUB_ASSET_NAME_LEN];
    char asset_url[OTA_GITHUB_URL_LEN];
    char release_url[OTA_GITHUB_URL_LEN];
} ota_release_info_t;

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} http_buffer_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    ota_state_t state;
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
} ota_ctx_t;

typedef struct {
    httpd_handle_t server;
    provisioning_web_callbacks_t callbacks;
    void *user_ctx;
    char device_id[DEVICE_ID_LEN];
    ota_ctx_t ota;
} web_ctx_t;

static web_ctx_t s_ctx;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static bool ota_lock_take(TickType_t timeout_ticks)
{
    if (s_ctx.ota.lock == NULL) {
        return false;
    }
    return xSemaphoreTake(s_ctx.ota.lock, timeout_ticks) == pdTRUE;
}

static void ota_lock_give(void)
{
    if (s_ctx.ota.lock != NULL) {
        xSemaphoreGive(s_ctx.ota.lock);
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
    ota_format_text(s_ctx.ota.status_text, sizeof(s_ctx.ota.status_text), fmt, args);
    va_end(args);
}

static void ota_set_error_text_locked(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ota_format_text(s_ctx.ota.last_error, sizeof(s_ctx.ota.last_error), fmt, args);
    va_end(args);
}

static bool github_ota_is_configured(void)
{
#if CONFIG_AIRMON_GITHUB_OTA_ENABLED
    return CONFIG_AIRMON_GITHUB_RELEASE_OWNER[0] != '\0' &&
           CONFIG_AIRMON_GITHUB_RELEASE_REPO[0] != '\0' &&
           CONFIG_AIRMON_GITHUB_OTA_ASSET_PREFIX[0] != '\0';
#else
    return false;
#endif
}

static const char *github_repo_name(void)
{
#if CONFIG_AIRMON_GITHUB_OTA_ENABLED
    return CONFIG_AIRMON_GITHUB_RELEASE_OWNER "/" CONFIG_AIRMON_GITHUB_RELEASE_REPO;
#else
    return "";
#endif
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

static bool text_starts_with(const char *text, const char *prefix)
{
    if (text == NULL || prefix == NULL) {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    return strncmp(text, prefix, prefix_len) == 0;
}

static bool text_ends_with(const char *text, const char *suffix)
{
    if (text == NULL || suffix == NULL) {
        return false;
    }
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len) {
        return false;
    }
    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

static bool normalize_version_text(const char *input, char *buffer, size_t buffer_len)
{
    if (input == NULL || buffer == NULL || buffer_len == 0) {
        return false;
    }

    while (*input != '\0' && isspace((unsigned char)*input)) {
        input++;
    }
    if (*input == 'v' || *input == 'V') {
        input++;
    }
    if (*input == '\0') {
        buffer[0] = '\0';
        return false;
    }

    size_t used = 0;
    while (input[used] != '\0' && !isspace((unsigned char)input[used])) {
        if (used + 1 >= buffer_len) {
            buffer[0] = '\0';
            return false;
        }
        buffer[used] = input[used];
        used++;
    }
    buffer[used] = '\0';
    return used > 0;
}

static bool parse_version_triplet(const char *text, version_triplet_t *version)
{
    if (text == NULL || version == NULL) {
        return false;
    }

    char normalized[FIRMWARE_VERSION_LEN];
    if (!normalize_version_text(text, normalized, sizeof(normalized))) {
        return false;
    }

    const char *cursor = normalized;
    char *end = NULL;

    long major = strtol(cursor, &end, 10);
    if (end == cursor || *end != '.') {
        return false;
    }
    cursor = end + 1;

    long minor = strtol(cursor, &end, 10);
    if (end == cursor || *end != '.') {
        return false;
    }
    cursor = end + 1;

    long patch = strtol(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }
    if (*end != '\0' && *end != '-' && *end != '+') {
        return false;
    }

    version->major = (int)major;
    version->minor = (int)minor;
    version->patch = (int)patch;
    return true;
}

static int compare_versions(const char *left, const char *right)
{
    version_triplet_t lhs = {0};
    version_triplet_t rhs = {0};
    bool lhs_ok = parse_version_triplet(left, &lhs);
    bool rhs_ok = parse_version_triplet(right, &rhs);
    if (lhs_ok && rhs_ok) {
        if (lhs.major != rhs.major) {
            return lhs.major < rhs.major ? -1 : 1;
        }
        if (lhs.minor != rhs.minor) {
            return lhs.minor < rhs.minor ? -1 : 1;
        }
        if (lhs.patch != rhs.patch) {
            return lhs.patch < rhs.patch ? -1 : 1;
        }
        return 0;
    }

    char lhs_text[FIRMWARE_VERSION_LEN] = {0};
    char rhs_text[FIRMWARE_VERSION_LEN] = {0};
    if (!normalize_version_text(left, lhs_text, sizeof(lhs_text)) ||
        !normalize_version_text(right, rhs_text, sizeof(rhs_text))) {
        return 0;
    }
    return strcmp(lhs_text, rhs_text);
}

static void ota_clear_release_info_locked(void)
{
    s_ctx.ota.update_available = false;
    s_ctx.ota.latest_version[0] = '\0';
    s_ctx.ota.release_tag[0] = '\0';
    s_ctx.ota.asset_name[0] = '\0';
    s_ctx.ota.asset_url[0] = '\0';
    s_ctx.ota.release_url[0] = '\0';
}

static void ota_store_release_info_locked(const ota_release_info_t *info, bool update_available)
{
    ota_clear_release_info_locked();
    if (info == NULL) {
        return;
    }

    s_ctx.ota.update_available = update_available;
    strlcpy(s_ctx.ota.latest_version, info->version, sizeof(s_ctx.ota.latest_version));
    strlcpy(s_ctx.ota.release_tag, info->tag, sizeof(s_ctx.ota.release_tag));
    strlcpy(s_ctx.ota.asset_name, info->asset_name, sizeof(s_ctx.ota.asset_name));
    strlcpy(s_ctx.ota.asset_url, info->asset_url, sizeof(s_ctx.ota.asset_url));
    strlcpy(s_ctx.ota.release_url, info->release_url, sizeof(s_ctx.ota.release_url));
}

static void ota_prepare_locked(ota_state_t state, const char *status_text)
{
    s_ctx.ota.state = state;
    s_ctx.ota.busy = true;
    s_ctx.ota.restart_pending = false;
    s_ctx.ota.progress_percent = 0;
    s_ctx.ota.bytes_read = 0;
    s_ctx.ota.total_bytes = 0;
    s_ctx.ota.last_updated_at_ms = 0;
    s_ctx.ota.last_error[0] = '\0';
    ota_set_status_text_locked("%s", status_text != NULL ? status_text : "");
}

static void ota_finish_ready_locked(const ota_release_info_t *info)
{
    ota_store_release_info_locked(info, true);
    s_ctx.ota.busy = false;
    s_ctx.ota.state = OTA_STATE_READY;
    s_ctx.ota.progress_percent = 0;
    s_ctx.ota.last_checked_at_ms = esp_timer_get_time() / 1000;
    ota_set_status_text_locked("检测到新版本 %s，可直接升级", s_ctx.ota.latest_version);
}

static void ota_finish_up_to_date_locked(const ota_release_info_t *info)
{
    ota_store_release_info_locked(info, false);
    s_ctx.ota.busy = false;
    s_ctx.ota.state = OTA_STATE_IDLE;
    s_ctx.ota.progress_percent = 0;
    s_ctx.ota.last_checked_at_ms = esp_timer_get_time() / 1000;
    ota_set_status_text_locked("当前已经是 GitHub 最新版本");
}

static void ota_finish_success_locked(const char *status_text)
{
    s_ctx.ota.busy = false;
    s_ctx.ota.state = OTA_STATE_SUCCESS;
    s_ctx.ota.restart_pending = true;
    s_ctx.ota.progress_percent = 100;
    s_ctx.ota.last_updated_at_ms = esp_timer_get_time() / 1000;
    s_ctx.ota.last_error[0] = '\0';
    ota_set_status_text_locked("%s", status_text != NULL ? status_text : "固件升级完成，即将重启");
}

static void ota_finish_error_locked(const char *message)
{
    s_ctx.ota.busy = false;
    s_ctx.ota.state = OTA_STATE_ERROR;
    s_ctx.ota.restart_pending = false;
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
    s_ctx.ota.bytes_read = bytes_read;
    s_ctx.ota.total_bytes = total_bytes;
    if (total_bytes > 0 && bytes_read <= total_bytes) {
        s_ctx.ota.progress_percent = (int)((bytes_read * 100U) / total_bytes);
    }
    if (status_text != NULL && *status_text != '\0') {
        ota_set_status_text_locked("%s", status_text);
    }
}

static void http_buffer_free(http_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->capacity = 0;
}

static esp_err_t http_buffer_reserve(http_buffer_t *buffer, size_t required)
{
    if (buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (required <= buffer->capacity) {
        return ESP_OK;
    }

    size_t new_capacity = buffer->capacity == 0 ? 2048 : buffer->capacity;
    while (new_capacity < required) {
        new_capacity *= 2;
    }

    char *new_data = realloc(buffer->data, new_capacity);
    if (new_data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return ESP_OK;
}

static esp_err_t http_collect_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->user_data == NULL) {
        return ESP_OK;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    http_buffer_t *buffer = (http_buffer_t *)evt->user_data;
    size_t required = buffer->len + (size_t)evt->data_len + 1;
    ESP_RETURN_ON_ERROR(http_buffer_reserve(buffer, required), TAG, "http buffer grow failed");
    memcpy(buffer->data + buffer->len, evt->data, (size_t)evt->data_len);
    buffer->len += (size_t)evt->data_len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static bool github_json_message(const char *payload, char *buffer, size_t buffer_len)
{
    if (payload == NULL || buffer == NULL || buffer_len == 0) {
        return false;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return false;
    }
    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    bool ok = cJSON_IsString(message) && message->valuestring[0] != '\0';
    if (ok) {
        strlcpy(buffer, message->valuestring, buffer_len);
    }
    cJSON_Delete(root);
    return ok;
}

static bool github_parse_release_info(const char *payload, ota_release_info_t *info, char *error, size_t error_len)
{
    if (payload == NULL || info == NULL) {
        snprintf(error, error_len, "GitHub Release 响应为空");
        return false;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        snprintf(error, error_len, "GitHub Release JSON 解析失败");
        return false;
    }

    cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    cJSON *html_url = cJSON_GetObjectItemCaseSensitive(root, "html_url");
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (!cJSON_IsString(tag) || tag->valuestring[0] == '\0') {
        cJSON_Delete(root);
        snprintf(error, error_len, "GitHub Release 缺少 tag_name");
        return false;
    }
    if (!normalize_version_text(tag->valuestring, info->version, sizeof(info->version))) {
        cJSON_Delete(root);
        snprintf(error, error_len, "GitHub Release 版本号格式无效");
        return false;
    }
    strlcpy(info->tag, tag->valuestring, sizeof(info->tag));
    if (cJSON_IsString(html_url)) {
        strlcpy(info->release_url, html_url->valuestring, sizeof(info->release_url));
    }

    char expected_name[OTA_GITHUB_ASSET_NAME_LEN];
    snprintf(expected_name, sizeof(expected_name), "%s%s.bin", CONFIG_AIRMON_GITHUB_OTA_ASSET_PREFIX, info->version);

    cJSON *selected = NULL;
    if (cJSON_IsArray(assets)) {
        cJSON *asset = NULL;
        cJSON_ArrayForEach(asset, assets) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
            cJSON *download = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            if (!cJSON_IsString(name) || !cJSON_IsString(download)) {
                continue;
            }
            if (strcmp(name->valuestring, expected_name) == 0) {
                selected = asset;
                break;
            }
            if (selected == NULL &&
                text_starts_with(name->valuestring, CONFIG_AIRMON_GITHUB_OTA_ASSET_PREFIX) &&
                text_ends_with(name->valuestring, ".bin")) {
                selected = asset;
            }
        }
    }

    if (selected == NULL) {
        cJSON_Delete(root);
        snprintf(error, error_len, "未找到 GitHub OTA 资产 %s", expected_name);
        return false;
    }

    cJSON *asset_name = cJSON_GetObjectItemCaseSensitive(selected, "name");
    cJSON *asset_url = cJSON_GetObjectItemCaseSensitive(selected, "browser_download_url");
    strlcpy(info->asset_name, asset_name->valuestring, sizeof(info->asset_name));
    strlcpy(info->asset_url, asset_url->valuestring, sizeof(info->asset_url));

    cJSON_Delete(root);
    return true;
}

static esp_err_t github_fetch_latest_release(ota_release_info_t *info, char *error, size_t error_len)
{
    if (info == NULL) {
        snprintf(error, error_len, "GitHub Release 目标缓冲区为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (!github_ota_is_configured()) {
        snprintf(error, error_len, "当前固件未启用 GitHub 自动更新");
        return ESP_ERR_INVALID_STATE;
    }

    char api_url[OTA_GITHUB_URL_LEN];
    snprintf(api_url,
             sizeof(api_url),
             "https://api.github.com/repos/%s/%s/releases/latest",
             CONFIG_AIRMON_GITHUB_RELEASE_OWNER,
             CONFIG_AIRMON_GITHUB_RELEASE_REPO);

    http_buffer_t response = {0};
    esp_http_client_config_t config = {
        .url = api_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = OTA_HTTP_USER_AGENT,
        .event_handler = http_collect_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(error, error_len, "GitHub HTTP 客户端初始化失败");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    if (err != ESP_OK) {
        snprintf(error, error_len, "GitHub Release 查询失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        http_buffer_free(&response);
        return err;
    }
    if (status_code < 200 || status_code >= 300) {
        if (!github_json_message(response.data, error, error_len)) {
            snprintf(error, error_len, "GitHub Release 查询返回 HTTP %d", status_code);
        }
        esp_http_client_cleanup(client);
        http_buffer_free(&response);
        return ESP_FAIL;
    }

    memset(info, 0, sizeof(*info));
    bool parsed = github_parse_release_info(response.data, info, error, error_len);
    esp_http_client_cleanup(client);
    http_buffer_free(&response);
    return parsed ? ESP_OK : ESP_FAIL;
}

static esp_err_t github_ota_http_client_init(esp_http_client_handle_t client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "ota http client null");
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    return ESP_OK;
}

static esp_err_t github_download_and_apply_ota(const ota_release_info_t *info, char *error, size_t error_len)
{
    if (info == NULL || info->asset_url[0] == '\0') {
        snprintf(error, error_len, "GitHub OTA 下载地址为空");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t http_config = {
        .url = info->asset_url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = OTA_HTTP_USER_AGENT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .http_client_init_cb = github_ota_http_client_init,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        snprintf(error, error_len, "GitHub OTA 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    while ((err = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int image_size = esp_https_ota_get_image_size(ota_handle);
        int image_len_read = esp_https_ota_get_image_len_read(ota_handle);
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_update_progress_locked(image_len_read > 0 ? (size_t)image_len_read : 0,
                                       image_size > 0 ? (size_t)image_size : 0,
                                       "正在从 GitHub 下载并写入固件...");
            ota_lock_give();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err != ESP_OK) {
        int status_code = esp_https_ota_get_status_code(ota_handle);
        esp_https_ota_abort(ota_handle);
        if (status_code > 0) {
            snprintf(error, error_len, "GitHub OTA 下载失败，HTTP %d", status_code);
        } else {
            snprintf(error, error_len, "GitHub OTA 下载失败: %s", esp_err_to_name(err));
        }
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        esp_https_ota_abort(ota_handle);
        snprintf(error, error_len, "GitHub OTA 下载未完成");
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        snprintf(error, error_len, "GitHub OTA 校验失败: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static void ota_mark_task_finished(void)
{
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        s_ctx.ota.task = NULL;
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

    esp_err_t err = github_fetch_latest_release(&info, error, sizeof(error));
    if (err != ESP_OK) {
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_finish_error_locked(error);
            ota_lock_give();
        }
        ota_mark_task_finished();
        vTaskDelete(NULL);
        return;
    }

    bool update_available = compare_versions(current_version, info.version) < 0;
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        if (update_available) {
            ota_store_release_info_locked(&info, true);
            ota_prepare_locked(OTA_STATE_DOWNLOADING, "正在从 GitHub 下载 OTA 固件...");
            strlcpy(s_ctx.ota.latest_version, info.version, sizeof(s_ctx.ota.latest_version));
            strlcpy(s_ctx.ota.release_tag, info.tag, sizeof(s_ctx.ota.release_tag));
            strlcpy(s_ctx.ota.asset_name, info.asset_name, sizeof(s_ctx.ota.asset_name));
            strlcpy(s_ctx.ota.asset_url, info.asset_url, sizeof(s_ctx.ota.asset_url));
            strlcpy(s_ctx.ota.release_url, info.release_url, sizeof(s_ctx.ota.release_url));
            s_ctx.ota.last_checked_at_ms = esp_timer_get_time() / 1000;
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

    err = github_download_and_apply_ota(&info, error, sizeof(error));
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        if (err == ESP_OK) {
            ota_finish_success_locked("GitHub OTA 下载完成，设备即将重启");
        } else {
            ota_finish_error_locked(error);
        }
        ota_lock_give();
    }

    ota_mark_task_finished();
    if (err == ESP_OK && s_ctx.callbacks.request_restart != NULL) {
        s_ctx.callbacks.request_restart(s_ctx.user_ctx);
    }
    vTaskDelete(NULL);
}

static void ota_append_status_json(cJSON *root, const char *current_version)
{
    cJSON *ota_json = cJSON_AddObjectToObject(root, "ota");
    if (ota_json == NULL) {
        ESP_LOGE(TAG, "ota json alloc failed");
        return;
    }

    ota_ctx_t snapshot = {0};
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        snapshot = s_ctx.ota;
        ota_lock_give();
    }

    cJSON_AddBoolToObject(ota_json, "enabled", snapshot.enabled);
    cJSON_AddBoolToObject(ota_json, "busy", snapshot.busy);
    cJSON_AddBoolToObject(ota_json, "update_available", snapshot.update_available);
    cJSON_AddBoolToObject(ota_json, "restart_pending", snapshot.restart_pending);
    cJSON_AddStringToObject(ota_json, "state", ota_state_key(snapshot.state));
    cJSON_AddStringToObject(ota_json, "current_version", current_version != NULL ? current_version : "");
    cJSON_AddStringToObject(ota_json, "latest_version", snapshot.latest_version);
    cJSON_AddStringToObject(ota_json, "release_tag", snapshot.release_tag);
    cJSON_AddStringToObject(ota_json, "asset_name", snapshot.asset_name);
    cJSON_AddStringToObject(ota_json, "asset_url", snapshot.asset_url);
    cJSON_AddStringToObject(ota_json, "release_url", snapshot.release_url);
    cJSON_AddStringToObject(ota_json, "status_text", snapshot.status_text);
    cJSON_AddStringToObject(ota_json, "last_error", snapshot.last_error);
    cJSON_AddStringToObject(ota_json, "github_repo", github_repo_name());
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
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_NO_MEM, TAG, "json render failed");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

static char *read_body(httpd_req_t *req)
{
    if (req->content_len > MAX_REQUEST_BODY_LEN) {
        return NULL;
    }
    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        return NULL;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return NULL;
        }
        received += ret;
    }
    body[received] = '\0';
    return body;
}

static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "json alloc failed");
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", message);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_ok_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t send_ok_restart_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"restart\":true}");
}

static esp_err_t send_ok_config_json(httpd_req_t *req, bool restart, bool runtime_applied)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "json alloc failed");
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddBoolToObject(root, "restart", restart);
    cJSON_AddBoolToObject(root, "runtime_applied", runtime_applied);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_ota_status_json(httpd_req_t *req, const char *current_version)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "json alloc failed");
    cJSON_AddStringToObject(root, "status", "ok");
    ota_append_status_json(root, current_version);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t parse_json_request(httpd_req_t *req, cJSON **json_out)
{
    char *body = read_body(req);
    if (body == NULL) {
        return send_error_json(req, "400 Bad Request", "请求体缺失或被截断");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_error_json(req, "400 Bad Request", "JSON 格式无效");
    }

    *json_out = json;
    return ESP_OK;
}

static bool number_in_range(cJSON *item, double min, double max)
{
    return cJSON_IsNumber(item) && !isnan(item->valuedouble) &&
           item->valuedouble >= min && item->valuedouble <= max;
}

static bool whole_number_in_range(cJSON *item, long min, long max)
{
    return number_in_range(item, (double)min, (double)max) &&
           floor(item->valuedouble) == item->valuedouble;
}

static void trim_ascii_whitespace(char *text)
{
    if (text == NULL) {
        return;
    }

    char *start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

static int mqtt_url_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static bool mqtt_url_decode_component(const char *src, char *dst, size_t dst_len)
{
    size_t used = 0;
    for (size_t i = 0; src != NULL && src[i] != '\0'; i++) {
        unsigned char value = (unsigned char)src[i];
        if (value == '%') {
            if (src[i + 1] == '\0' || src[i + 2] == '\0') {
                return false;
            }
            int hi = mqtt_url_hex_value(src[i + 1]);
            int lo = mqtt_url_hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            value = (unsigned char)((hi << 4) | lo);
            i += 2;
        }

        if (used + 1 >= dst_len) {
            return false;
        }
        dst[used++] = (char)value;
    }

    if (dst_len == 0) {
        return false;
    }
    dst[used] = '\0';
    return true;
}

static bool parse_mqtt_port_text(const char *text, uint16_t *port_out)
{
    if (text == NULL || *text == '\0' || port_out == NULL) {
        return false;
    }

    char *end = NULL;
    long port = strtol(text, &end, 10);
    if (end == NULL || *end != '\0' || port < MQTT_PORT_MIN || port > MQTT_PORT_MAX) {
        return false;
    }

    *port_out = (uint16_t)port;
    return true;
}

static void clear_mqtt_url_config(device_config_t *config)
{
    config->mqtt_host[0] = '\0';
    config->mqtt_username[0] = '\0';
    config->mqtt_password[0] = '\0';
    config->mqtt_port = MQTT_DEFAULT_PORT;
}

static bool parse_mqtt_url_string(const char *input, device_config_t *config, char *error, size_t error_len)
{
    char url[MQTT_URL_BUFFER_LEN];
    if (input == NULL) {
        clear_mqtt_url_config(config);
        return true;
    }

    size_t length = strlen(input);
    if (length >= sizeof(url)) {
        snprintf(error, error_len, "MQTT URL 过长");
        return false;
    }

    memcpy(url, input, length + 1);
    trim_ascii_whitespace(url);
    if (url[0] == '\0') {
        clear_mqtt_url_config(config);
        return true;
    }

    const size_t scheme_len = strlen(MQTT_SCHEME);
    if (strncmp(url, MQTT_SCHEME, scheme_len) != 0) {
        snprintf(error, error_len, "MQTT URL 必须以 mqtt:// 开头");
        return false;
    }

    char *authority = url + scheme_len;
    char *tail = strpbrk(authority, "/?#");
    if (tail != NULL) {
        if (*tail == '/' && tail[1] == '\0') {
            *tail = '\0';
        } else {
            snprintf(error, error_len, "MQTT URL 只支持地址、端口、用户名和密码");
            return false;
        }
    }
    if (authority[0] == '\0') {
        snprintf(error, error_len, "MQTT URL 缺少主机地址");
        return false;
    }

    char username[MQTT_USER_LEN + 1] = {0};
    char password[MQTT_PASSWORD_LEN + 1] = {0};
    char host[MQTT_HOST_LEN + 1] = {0};
    uint16_t port = MQTT_DEFAULT_PORT;

    char *host_part = authority;
    char *userinfo = strrchr(authority, '@');
    if (userinfo != NULL) {
        *userinfo = '\0';
        host_part = userinfo + 1;

        char *password_sep = strchr(authority, ':');
        if (password_sep != NULL) {
            *password_sep = '\0';
            if (!mqtt_url_decode_component(authority, username, sizeof(username)) ||
                !mqtt_url_decode_component(password_sep + 1, password, sizeof(password))) {
                snprintf(error, error_len, "MQTT 用户名或密码包含无效的 URL 编码");
                return false;
            }
        } else if (!mqtt_url_decode_component(authority, username, sizeof(username))) {
            snprintf(error, error_len, "MQTT 用户名包含无效的 URL 编码");
            return false;
        }
    }

    if (host_part[0] == '[') {
        char *host_end = strchr(host_part, ']');
        if (host_end == NULL) {
            snprintf(error, error_len, "MQTT 主机地址格式无效");
            return false;
        }

        size_t host_len = (size_t)(host_end - host_part + 1);
        if (host_len == 0 || host_len > MQTT_HOST_LEN) {
            snprintf(error, error_len, "MQTT 主机地址过长");
            return false;
        }
        memcpy(host, host_part, host_len);
        host[host_len] = '\0';

        if (host_end[1] == '\0') {
            port = MQTT_DEFAULT_PORT;
        } else if (host_end[1] == ':') {
            if (!parse_mqtt_port_text(host_end + 2, &port)) {
                snprintf(error, error_len, "MQTT 端口必须在 1 到 65535 之间");
                return false;
            }
        } else {
            snprintf(error, error_len, "MQTT 主机地址格式无效");
            return false;
        }
    } else {
        char *port_sep = strrchr(host_part, ':');
        if (port_sep != NULL) {
            if (strchr(host_part, ':') != port_sep) {
                snprintf(error, error_len, "IPv6 地址请使用 [addr]:port 格式");
                return false;
            }
            *port_sep = '\0';
            if (!parse_mqtt_port_text(port_sep + 1, &port)) {
                snprintf(error, error_len, "MQTT 端口必须在 1 到 65535 之间");
                return false;
            }
        }

        if (host_part[0] == '\0') {
            snprintf(error, error_len, "MQTT URL 缺少主机地址");
            return false;
        }
        if (strlen(host_part) > MQTT_HOST_LEN) {
            snprintf(error, error_len, "MQTT 主机地址过长");
            return false;
        }
        strlcpy(host, host_part, sizeof(host));
    }

    strlcpy(config->mqtt_host, host, sizeof(config->mqtt_host));
    strlcpy(config->mqtt_username, username, sizeof(config->mqtt_username));
    strlcpy(config->mqtt_password, password, sizeof(config->mqtt_password));
    config->mqtt_port = port;
    return true;
}

static void build_public_mqtt_url_string(const device_config_t *config, char *buffer, size_t buffer_len)
{
    if (buffer_len == 0) {
        return;
    }

    buffer[0] = '\0';
    if (config == NULL || config->mqtt_host[0] == '\0') {
        return;
    }

    uint16_t port = config->mqtt_port ? config->mqtt_port : MQTT_DEFAULT_PORT;
    int written = snprintf(buffer, buffer_len, MQTT_SCHEME "%s:%u", config->mqtt_host, port);
    if (written < 0 || (size_t)written >= buffer_len) {
        buffer[0] = '\0';
    }
}

static void fill_status(sensor_snapshot_t *snapshot, device_diag_t *diag, device_config_t *config, uint16_t *frc_ppm)
{
    memset(snapshot, 0, sizeof(*snapshot));
    memset(diag, 0, sizeof(*diag));
    memset(config, 0, sizeof(*config));
    *frc_ppm = 400;
    if (s_ctx.callbacks.get_status != NULL) {
        s_ctx.callbacks.get_status(snapshot, diag, config, frc_ppm, s_ctx.user_ctx);
    }
}

static const char *co2_compensation_source_key(co2_compensation_source_t source)
{
    switch (source) {
    case CO2_COMPENSATION_SOURCE_ALTITUDE:
        return "altitude";
    case CO2_COMPENSATION_SOURCE_BMP390:
        return "bmp390";
    case CO2_COMPENSATION_SOURCE_NONE:
    default:
        return "none";
    }
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const size_t index_html_size = (index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, index_html_size);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    sensor_snapshot_t snapshot;
    device_diag_t diag;
    device_config_t config;
    uint16_t frc_ppm;
    char mqtt_url[MQTT_URL_BUFFER_LEN];
    fill_status(&snapshot, &diag, &config, &frc_ppm);
    air_quality_assessment_t assessment = {0};
    air_quality_compute_overall_assessment(&snapshot, &assessment);
    air_quality_particle_insight_t particle = {0};
    air_quality_compute_particle_insight(&snapshot, &particle);
    air_quality_rain_analysis_t rain = {0};
    air_quality_analyze_rain(&snapshot, &rain);
    int64_t now_ms = esp_timer_get_time() / 1000;
    build_public_mqtt_url_string(&config, mqtt_url, sizeof(mqtt_url));

    cJSON *root = cJSON_CreateObject();
    cJSON *diag_json = cJSON_AddObjectToObject(root, "diag");
    cJSON_AddBoolToObject(diag_json, "provisioning_mode", diag.provisioning_mode);
    cJSON_AddBoolToObject(diag_json, "wifi_connected", diag.wifi_connected);
    cJSON_AddBoolToObject(diag_json, "mqtt_connected", diag.mqtt_connected);
    cJSON_AddBoolToObject(diag_json, "sensors_ready", diag.sensors_ready);
    cJSON_AddBoolToObject(diag_json, "scd41_ready", diag.scd41_ready);
    cJSON_AddBoolToObject(diag_json, "sgp41_ready", diag.sgp41_ready);
    cJSON_AddBoolToObject(diag_json, "bmp390_ready", diag.bmp390_ready);
    cJSON_AddBoolToObject(diag_json, "sps30_ready", diag.sps30_ready);
    cJSON_AddBoolToObject(diag_json, "status_led_ready", diag.status_led_ready);
    cJSON_AddBoolToObject(diag_json, "status_led_enabled", diag.status_led_enabled);
    cJSON_AddNumberToObject(diag_json, "wifi_rssi", diag.wifi_rssi);
    cJSON_AddNumberToObject(diag_json, "uptime_sec", diag.uptime_sec);
    cJSON_AddNumberToObject(diag_json, "heap_free", diag.heap_free);
    cJSON_AddStringToObject(diag_json, "ip_addr", diag.ip_addr);
    cJSON_AddStringToObject(diag_json, "device_id", diag.device_id);
    cJSON_AddStringToObject(diag_json, "firmware_version", diag.firmware_version);
    cJSON_AddStringToObject(diag_json, "last_error", diag.last_error[0] ? diag.last_error : "none");

    cJSON *snapshot_json = cJSON_AddObjectToObject(root, "snapshot");
    cJSON_AddBoolToObject(snapshot_json, "scd41_valid", snapshot.scd41_valid);
    cJSON_AddBoolToObject(snapshot_json, "sgp41_valid", snapshot.sgp41_valid);
    cJSON_AddBoolToObject(snapshot_json, "sgp41_conditioning", snapshot.sgp41_conditioning);
    cJSON_AddBoolToObject(snapshot_json, "sgp41_voc_valid", snapshot.sgp41_voc_valid);
    cJSON_AddBoolToObject(snapshot_json, "sgp41_nox_valid", snapshot.sgp41_nox_valid);
    cJSON_AddBoolToObject(snapshot_json, "bmp390_valid", snapshot.bmp390_valid);
    cJSON_AddBoolToObject(snapshot_json, "pm_valid", snapshot.pm_valid);
    cJSON_AddStringToObject(snapshot_json, "co2_compensation_source",
                            co2_compensation_source_key(snapshot.co2_compensation_source));
    cJSON_AddNumberToObject(snapshot_json,
                            "sgp41_voc_stabilization_remaining_s",
                            snapshot.sgp41_voc_stabilization_remaining_s);
    cJSON_AddNumberToObject(snapshot_json,
                            "sgp41_nox_stabilization_remaining_s",
                            snapshot.sgp41_nox_stabilization_remaining_s);
    if (snapshot.scd41_valid) {
        cJSON_AddNumberToObject(snapshot_json, "co2_ppm", snapshot.co2_ppm);
        cJSON_AddStringToObject(snapshot_json, "co2_rating",
                                air_quality_co2_ventilation_label(air_quality_rate_co2(snapshot.co2_ppm)));
        cJSON_AddNumberToObject(snapshot_json, "temperature_c", snapshot.temperature_c);
        cJSON_AddStringToObject(snapshot_json, "temperature_rating",
                                air_quality_rate_temperature_label(snapshot.temperature_c));
        cJSON_AddNumberToObject(snapshot_json, "humidity_rh", snapshot.humidity_rh);
        cJSON_AddStringToObject(snapshot_json, "humidity_rating",
                                air_quality_rate_humidity_label(snapshot.humidity_rh));
    } else {
        cJSON_AddNullToObject(snapshot_json, "co2_ppm");
        cJSON_AddNullToObject(snapshot_json, "co2_rating");
        cJSON_AddNullToObject(snapshot_json, "temperature_c");
        cJSON_AddNullToObject(snapshot_json, "temperature_rating");
        cJSON_AddNullToObject(snapshot_json, "humidity_rh");
        cJSON_AddNullToObject(snapshot_json, "humidity_rating");
    }
    if (snapshot.sgp41_voc_valid) {
        cJSON_AddNumberToObject(snapshot_json, "voc_index", snapshot.voc_index);
        cJSON_AddStringToObject(snapshot_json, "voc_rating",
                                air_quality_voc_event_label(air_quality_rate_voc_index(snapshot.voc_index)));
    } else {
        cJSON_AddNullToObject(snapshot_json, "voc_index");
        cJSON_AddNullToObject(snapshot_json, "voc_rating");
    }
    if (snapshot.sgp41_nox_valid) {
        cJSON_AddNumberToObject(snapshot_json, "nox_index", snapshot.nox_index);
        cJSON_AddStringToObject(snapshot_json, "nox_rating",
                                air_quality_nox_event_label(air_quality_rate_nox_index(snapshot.nox_index)));
    } else {
        cJSON_AddNullToObject(snapshot_json, "nox_index");
        cJSON_AddNullToObject(snapshot_json, "nox_rating");
    }
    if (snapshot.bmp390_valid) {
        cJSON_AddNumberToObject(snapshot_json, "bmp390_temperature_c", snapshot.bmp390_temperature_c);
        cJSON_AddNumberToObject(snapshot_json, "pressure_hpa", snapshot.pressure_hpa);
    } else {
        cJSON_AddNullToObject(snapshot_json, "bmp390_temperature_c");
        cJSON_AddNullToObject(snapshot_json, "pressure_hpa");
    }
    if (snapshot.pressure_trend_valid) {
        cJSON_AddNumberToObject(snapshot_json, "pressure_trend_hpa_3h", snapshot.pressure_trend_hpa_3h);
        cJSON_AddNumberToObject(snapshot_json, "pressure_trend_span_min", snapshot.pressure_trend_span_min);
    } else {
        cJSON_AddNullToObject(snapshot_json, "pressure_trend_hpa_3h");
        cJSON_AddNullToObject(snapshot_json, "pressure_trend_span_min");
    }
    if (snapshot.humidity_trend_valid) {
        cJSON_AddNumberToObject(snapshot_json, "humidity_trend_rh_3h", snapshot.humidity_trend_rh_3h);
        cJSON_AddNumberToObject(snapshot_json, "humidity_trend_span_min", snapshot.humidity_trend_span_min);
    } else {
        cJSON_AddNullToObject(snapshot_json, "humidity_trend_rh_3h");
        cJSON_AddNullToObject(snapshot_json, "humidity_trend_span_min");
    }
    if (rain.dew_point_spread_valid) {
        cJSON_AddNumberToObject(snapshot_json, "dew_point_spread_c", rain.dew_point_spread_c);
    } else {
        cJSON_AddNullToObject(snapshot_json, "dew_point_spread_c");
    }
    cJSON_AddStringToObject(snapshot_json, "pressure_trend", air_quality_pressure_trend_label(rain.pressure_trend));
    cJSON_AddStringToObject(snapshot_json, "pressure_trend_key", air_quality_pressure_trend_key(rain.pressure_trend));
    cJSON_AddStringToObject(snapshot_json, "rain_outlook", air_quality_rain_outlook_label(rain.outlook));
    cJSON_AddStringToObject(snapshot_json, "rain_outlook_key", air_quality_rain_outlook_key(rain.outlook));
    cJSON_AddStringToObject(snapshot_json, "rain_season", air_quality_rain_season_label(rain.season));
    cJSON_AddStringToObject(snapshot_json, "rain_season_key", air_quality_rain_season_key(rain.season));
    cJSON_AddStringToObject(snapshot_json, "rain_basis", rain.basis[0] ? rain.basis : "Unavailable");
    if (snapshot.pm_valid) {
        cJSON_AddNumberToObject(snapshot_json, "pm1_0", snapshot.pm1_0);
        cJSON_AddNumberToObject(snapshot_json, "pm2_5", snapshot.pm2_5);
        cJSON_AddNumberToObject(snapshot_json, "pm4_0", snapshot.pm4_0);
        cJSON_AddNumberToObject(snapshot_json, "pm10_0", snapshot.pm10_0);
        cJSON_AddStringToObject(snapshot_json, "particle_profile",
                                air_quality_particle_profile_label(particle.profile));
        cJSON_AddStringToObject(snapshot_json, "particle_profile_key",
                                air_quality_particle_profile_key(particle.profile));
        cJSON_AddStringToObject(snapshot_json, "particle_profile_note",
                                particle.note[0] ? particle.note : "Unavailable");
        cJSON_AddNumberToObject(snapshot_json, "particles_0_5um", snapshot.particles_0_5um);
        cJSON_AddNumberToObject(snapshot_json, "particles_1_0um", snapshot.particles_1_0um);
        cJSON_AddNumberToObject(snapshot_json, "particles_2_5um", snapshot.particles_2_5um);
        cJSON_AddNumberToObject(snapshot_json, "particles_4_0um", snapshot.particles_4_0um);
        cJSON_AddNumberToObject(snapshot_json, "particles_10_0um", snapshot.particles_10_0um);
        cJSON_AddNumberToObject(snapshot_json, "typical_particle_size_um", snapshot.typical_particle_size_um);
    } else {
        cJSON_AddNullToObject(snapshot_json, "pm1_0");
        cJSON_AddNullToObject(snapshot_json, "pm2_5");
        cJSON_AddNullToObject(snapshot_json, "pm4_0");
        cJSON_AddNullToObject(snapshot_json, "pm10_0");
        cJSON_AddNullToObject(snapshot_json, "particle_profile");
        cJSON_AddNullToObject(snapshot_json, "particle_profile_key");
        cJSON_AddNullToObject(snapshot_json, "particle_profile_note");
        cJSON_AddNullToObject(snapshot_json, "particles_0_5um");
        cJSON_AddNullToObject(snapshot_json, "particles_1_0um");
        cJSON_AddNullToObject(snapshot_json, "particles_2_5um");
        cJSON_AddNullToObject(snapshot_json, "particles_4_0um");
        cJSON_AddNullToObject(snapshot_json, "particles_10_0um");
        cJSON_AddNullToObject(snapshot_json, "typical_particle_size_um");
    }
    if (snapshot.updated_at_ms > 0 && now_ms >= snapshot.updated_at_ms) {
        cJSON_AddNumberToObject(snapshot_json, "sample_age_sec", (now_ms - snapshot.updated_at_ms) / 1000);
    } else {
        cJSON_AddNullToObject(snapshot_json, "sample_age_sec");
    }
    cJSON_AddBoolToObject(snapshot_json, "sps30_sleeping", snapshot.sps30_sleeping);
    if (assessment.us_aqi.valid) {
        cJSON_AddNumberToObject(snapshot_json, "us_aqi", assessment.us_aqi.aqi);
        cJSON_AddStringToObject(snapshot_json, "us_aqi_level", air_quality_category_label(assessment.us_aqi.category));
        cJSON_AddStringToObject(snapshot_json, "us_aqi_level_key", air_quality_category_key(assessment.us_aqi.category));
        cJSON_AddStringToObject(snapshot_json, "us_aqi_primary_pollutant",
                                air_quality_pollutant_label(assessment.us_aqi.dominant_pollutant));
    } else {
        cJSON_AddNullToObject(snapshot_json, "us_aqi");
        cJSON_AddNullToObject(snapshot_json, "us_aqi_level");
        cJSON_AddStringToObject(snapshot_json, "us_aqi_level_key", air_quality_category_key(AIR_QUALITY_CATEGORY_UNKNOWN));
        cJSON_AddNullToObject(snapshot_json, "us_aqi_primary_pollutant");
    }
    if (assessment.valid) {
        cJSON_AddStringToObject(snapshot_json, "overall_air_quality", air_quality_category_label(assessment.category));
        cJSON_AddStringToObject(snapshot_json, "overall_air_quality_key", air_quality_category_key(assessment.category));
        cJSON_AddStringToObject(snapshot_json, "overall_air_quality_driver",
                                air_quality_factor_label(assessment.dominant_factor));
    } else {
        cJSON_AddNullToObject(snapshot_json, "overall_air_quality");
        cJSON_AddStringToObject(snapshot_json, "overall_air_quality_key",
                                air_quality_category_key(AIR_QUALITY_CATEGORY_UNKNOWN));
        cJSON_AddNullToObject(snapshot_json, "overall_air_quality_driver");
    }
    cJSON_AddStringToObject(snapshot_json, "overall_air_quality_basis",
                            assessment.basis[0] ? assessment.basis : "Unavailable");
    cJSON_AddStringToObject(snapshot_json, "overall_air_quality_note",
                            assessment.note[0] ? assessment.note : "Unavailable");

    cJSON *config_json = cJSON_AddObjectToObject(root, "config");
    cJSON_AddStringToObject(config_json, "mqtt_url", mqtt_url);
    cJSON_AddBoolToObject(config_json, "mqtt_auth_configured",
                          config.mqtt_username[0] != '\0' || config.mqtt_password[0] != '\0');
    cJSON_AddNumberToObject(config_json, "scd41_altitude_m", config.scd41_altitude_m);
    cJSON_AddNumberToObject(config_json, "scd41_temp_offset_c", config.scd41_temp_offset_c);
    cJSON_AddBoolToObject(config_json, "scd41_asc_enabled", config.scd41_asc_enabled);
    cJSON_AddNumberToObject(root, "frc_reference_ppm", frc_ppm);
    ota_append_status_json(root, diag.firmware_version);

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    sensor_snapshot_t snapshot;
    device_diag_t diag;
    device_config_t config;
    uint16_t frc_ppm;
    fill_status(&snapshot, &diag, &config, &frc_ppm);

    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *item = NULL;
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "device_name")) && cJSON_IsString(item)) {
        strlcpy(config.device_name, item->valuestring, sizeof(config.device_name));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "wifi_ssid")) && cJSON_IsString(item)) {
        strlcpy(config.wifi_ssid, item->valuestring, sizeof(config.wifi_ssid));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "wifi_password")) && cJSON_IsString(item)) {
        strlcpy(config.wifi_password, item->valuestring, sizeof(config.wifi_password));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_url")) && cJSON_IsString(item)) {
        char error[96];
        if (!parse_mqtt_url_string(item->valuestring, &config, error, sizeof(error))) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", error);
        }
    } else {
        if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_host")) && cJSON_IsString(item)) {
            strlcpy(config.mqtt_host, item->valuestring, sizeof(config.mqtt_host));
        }
        if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_port")) != NULL) {
            if (!whole_number_in_range(item, MQTT_PORT_MIN, MQTT_PORT_MAX)) {
                cJSON_Delete(json);
                return send_error_json(req, "400 Bad Request", "MQTT 端口必须在 1 到 65535 之间");
            }
            config.mqtt_port = (uint16_t)item->valuedouble;
        }
        if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_username")) && cJSON_IsString(item)) {
            strlcpy(config.mqtt_username, item->valuestring, sizeof(config.mqtt_username));
        }
        if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_password")) && cJSON_IsString(item)) {
            strlcpy(config.mqtt_password, item->valuestring, sizeof(config.mqtt_password));
        }
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "scd41_altitude_m")) != NULL) {
        if (!whole_number_in_range(item, SCD41_ALTITUDE_MIN, SCD41_ALTITUDE_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "SCD41 海拔补偿必须在 0 到 3000 米之间");
        }
        config.scd41_altitude_m = (uint16_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "scd41_temp_offset_c")) != NULL) {
        if (!number_in_range(item, SCD41_TEMP_OFFSET_MIN, SCD41_TEMP_OFFSET_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "SCD41 温度偏移必须在 0 到 20 摄氏度之间");
        }
        config.scd41_temp_offset_c = (float)item->valuedouble;
    }
    cJSON_Delete(json);

    bool restart_required = false;
    bool runtime_applied = false;
    if (s_ctx.callbacks.save_config != NULL) {
        esp_err_t err = s_ctx.callbacks.save_config(&config, &restart_required, &runtime_applied, s_ctx.user_ctx);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "SCD41 当前不可用，无法即时应用补偿参数");
        }
        if (err == ESP_ERR_INVALID_ARG) {
            return send_error_json(req, "400 Bad Request", "SCD41 补偿参数无效");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "save config failed");
    }

    return send_ok_config_json(req, restart_required, runtime_applied);
}

static esp_err_t simple_action_handler(httpd_req_t *req, void (*action)(void *))
{
    if (action != NULL) {
        action(s_ctx.user_ctx);
    }
    return send_ok_json(req);
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    return simple_action_handler(req, s_ctx.callbacks.request_restart);
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    return simple_action_handler(req, s_ctx.callbacks.request_factory_reset);
}

static esp_err_t republish_handler(httpd_req_t *req)
{
    if (s_ctx.callbacks.request_republish_discovery != NULL) {
        esp_err_t err = s_ctx.callbacks.request_republish_discovery(s_ctx.user_ctx);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "MQTT 未连接，无法重新发布 Discovery");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "republish discovery action failed");
    }
    return send_ok_json(req);
}

static esp_err_t scd41_asc_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "缺少有效的 enabled 布尔字段");
    }

    if (s_ctx.callbacks.request_set_scd41_asc != NULL) {
        esp_err_t err = s_ctx.callbacks.request_set_scd41_asc(cJSON_IsTrue(enabled), s_ctx.user_ctx);
        cJSON_Delete(json);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "SCD41 当前不可用");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "scd41 asc action failed");
        return send_ok_json(req);
    }

    cJSON_Delete(json);
    return send_ok_json(req);
}

static esp_err_t sps30_sleep_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *sleep = cJSON_GetObjectItemCaseSensitive(json, "sleep");
    if (!cJSON_IsBool(sleep)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "缺少有效的 sleep 布尔字段");
    }

    if (s_ctx.callbacks.request_set_sps30_sleep != NULL) {
        esp_err_t err = s_ctx.callbacks.request_set_sps30_sleep(cJSON_IsTrue(sleep), s_ctx.user_ctx);
        cJSON_Delete(json);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "SPS30 当前不可用");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "sps30 sleep action failed");
        return send_ok_json(req);
    }

    cJSON_Delete(json);
    return send_ok_json(req);
}

static esp_err_t sps30_fan_cleaning_handler(httpd_req_t *req)
{
    if (s_ctx.callbacks.request_start_sps30_fan_cleaning != NULL) {
        esp_err_t err = s_ctx.callbacks.request_start_sps30_fan_cleaning(s_ctx.user_ctx);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "SPS30 当前不可用，或正处于休眠中");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "sps30 fan cleaning action failed");
    }
    return send_ok_json(req);
}

static esp_err_t status_led_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "缺少有效的 enabled 布尔字段");
    }

    if (s_ctx.callbacks.request_set_status_led != NULL) {
        esp_err_t err = s_ctx.callbacks.request_set_status_led(cJSON_IsTrue(enabled), s_ctx.user_ctx);
        cJSON_Delete(json);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "RGB 状态灯当前不可用");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "status led action failed");
        return send_ok_json(req);
    }

    cJSON_Delete(json);
    return send_ok_json(req);
}

static esp_err_t frc_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *ppm = cJSON_GetObjectItemCaseSensitive(json, "ppm");
    if (!whole_number_in_range(ppm, 400, 2000)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "FRC 参考值必须在 400 到 2000 ppm 之间");
    }
    if (s_ctx.callbacks.request_apply_frc != NULL) {
        esp_err_t err = s_ctx.callbacks.request_apply_frc((uint16_t)ppm->valuedouble, s_ctx.user_ctx);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return send_error_json(req, "409 Conflict", "当前不满足 SCD41 FRC 的数据手册前置条件");
        }
    }
    cJSON_Delete(json);
    return send_ok_json(req);
}

static esp_err_t github_ota_check_handler(httpd_req_t *req)
{
    if (!github_ota_is_configured()) {
        return send_error_json(req, "503 Service Unavailable", "当前固件未启用 GitHub 自动更新");
    }
    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        return send_error_json(req, "500 Internal Server Error", "OTA 状态锁不可用");
    }
    if (s_ctx.ota.busy || s_ctx.ota.restart_pending) {
        ota_lock_give();
        return send_error_json(req, "409 Conflict", "OTA 升级正在进行中，请稍后再试");
    }
    ota_prepare_locked(OTA_STATE_CHECKING, "正在查询 GitHub 最新版本...");
    ota_lock_give();

    ota_release_info_t info = {0};
    char error[OTA_ERROR_TEXT_LEN] = {0};
    esp_err_t err = github_fetch_latest_release(&info, error, sizeof(error));
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_version = app_desc != NULL ? app_desc->version : "";

    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        return send_error_json(req, "500 Internal Server Error", "OTA 状态锁不可用");
    }
    if (err == ESP_OK) {
        if (compare_versions(current_version, info.version) < 0) {
            ota_finish_ready_locked(&info);
        } else {
            ota_finish_up_to_date_locked(&info);
        }
    } else {
        ota_finish_error_locked(error);
    }
    ota_lock_give();

    if (err != ESP_OK) {
        return send_error_json(req, "502 Bad Gateway", error[0] ? error : "GitHub Release 查询失败");
    }
    return send_ota_status_json(req, current_version);
}

static esp_err_t github_ota_update_handler(httpd_req_t *req)
{
    if (!github_ota_is_configured()) {
        return send_error_json(req, "503 Service Unavailable", "当前固件未启用 GitHub 自动更新");
    }
    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        return send_error_json(req, "500 Internal Server Error", "OTA 状态锁不可用");
    }
    if (s_ctx.ota.busy || s_ctx.ota.task != NULL || s_ctx.ota.restart_pending) {
        ota_lock_give();
        return send_error_json(req, "409 Conflict", "OTA 升级正在进行中，请稍后再试");
    }
    ota_prepare_locked(OTA_STATE_CHECKING, "正在查询 GitHub 最新版本...");
    ota_lock_give();

    TaskHandle_t task = NULL;
    if (xTaskCreate(github_ota_task,
                    "github_ota_task",
                    OTA_DOWNLOAD_TASK_STACK_SIZE,
                    NULL,
                    OTA_DOWNLOAD_TASK_PRIORITY,
                    &task) != pdPASS) {
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_finish_error_locked("无法启动 GitHub OTA 后台任务");
            ota_lock_give();
        }
        return send_error_json(req, "500 Internal Server Error", "无法启动 GitHub OTA 后台任务");
    }

    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        s_ctx.ota.task = task;
        ota_lock_give();
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    return send_ota_status_json(req, app_desc != NULL ? app_desc->version : "");
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        return send_error_json(req, "500 Internal Server Error", "未找到 OTA 分区");
    }
    if (req->content_len <= 0) {
        return send_error_json(req, "400 Bad Request", "固件文件为空");
    }
    if (req->content_len > OTA_MAX_FIRMWARE_SIZE || (size_t)req->content_len > partition->size) {
        return send_error_json(req, "400 Bad Request", "固件大小无效或超过 OTA 分区限制");
    }
    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        return send_error_json(req, "500 Internal Server Error", "OTA 状态锁不可用");
    }
    if (s_ctx.ota.busy || s_ctx.ota.task != NULL || s_ctx.ota.restart_pending) {
        ota_lock_give();
        return send_error_json(req, "409 Conflict", "OTA 升级正在进行中，请稍后再试");
    }
    ota_prepare_locked(OTA_STATE_UPLOAD, "正在上传本地固件...");
    s_ctx.ota.total_bytes = (size_t)req->content_len;
    ota_lock_give();

    esp_ota_handle_t ota_handle = 0;
    esp_err_t begin_err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (begin_err != ESP_OK) {
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_finish_error_locked("OTA 分区初始化失败");
            ota_lock_give();
        }
        return send_error_json(req, "500 Internal Server Error", "OTA 分区初始化失败");
    }

    char chunk[1024];
    int remaining = req->content_len;
    size_t bytes_written = 0;
    while (remaining > 0) {
        int read = httpd_req_recv(req, chunk, remaining > (int)sizeof(chunk) ? (int)sizeof(chunk) : remaining);
        if (read <= 0) {
            esp_ota_abort(ota_handle);
            if (ota_lock_take(pdMS_TO_TICKS(1000))) {
                ota_finish_error_locked("固件上传被中断，请重试");
                ota_lock_give();
            }
            return send_error_json(req, "500 Internal Server Error", "固件上传被中断，请重试");
        }
        esp_err_t write_err = esp_ota_write(ota_handle, chunk, read);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "ota write failed: %d", write_err);
            esp_ota_abort(ota_handle);
            if (ota_lock_take(pdMS_TO_TICKS(1000))) {
                ota_finish_error_locked("固件写入失败，请重试");
                ota_lock_give();
            }
            return send_error_json(req, "500 Internal Server Error", "固件写入失败，请重试");
        }
        remaining -= read;
        bytes_written += (size_t)read;
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_update_progress_locked(bytes_written, (size_t)req->content_len, "正在上传本地固件...");
            ota_lock_give();
        }
    }

    esp_err_t end_err = esp_ota_end(ota_handle);
    if (end_err != ESP_OK) {
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_finish_error_locked("固件校验失败，请确认上传的是 OTA 应用固件");
            ota_lock_give();
        }
        return send_error_json(req, "400 Bad Request", "固件校验失败，请确认上传的是 OTA 应用固件");
    }
    esp_err_t boot_err = esp_ota_set_boot_partition(partition);
    if (boot_err != ESP_OK) {
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_finish_error_locked("切换启动分区失败");
            ota_lock_give();
        }
        return send_error_json(req, "500 Internal Server Error", "切换启动分区失败");
    }
    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        ota_finish_success_locked("本地固件上传完成，设备即将重启");
        ota_lock_give();
    }
    if (s_ctx.callbacks.request_restart != NULL) {
        s_ctx.callbacks.request_restart(s_ctx.user_ctx);
    }
    return send_ok_restart_json(req);
}

esp_err_t provisioning_web_start(const char *device_id,
                                 const provisioning_web_callbacks_t *callbacks,
                                 void *user_ctx)
{
    provisioning_web_stop();
    if (s_ctx.ota.lock != NULL) {
        bool ota_running = false;
        if (xSemaphoreTake(s_ctx.ota.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ota_running = s_ctx.ota.task != NULL || s_ctx.ota.busy;
            xSemaphoreGive(s_ctx.ota.lock);
        }
        if (ota_running) {
            ESP_LOGW(TAG, "web start skipped because ota task is still running");
            return ESP_ERR_INVALID_STATE;
        }
        vSemaphoreDelete(s_ctx.ota.lock);
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    strlcpy(s_ctx.device_id, device_id, sizeof(s_ctx.device_id));
    if (callbacks != NULL) {
        s_ctx.callbacks = *callbacks;
    }
    s_ctx.user_ctx = user_ctx;
    s_ctx.ota.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.ota.lock != NULL, ESP_ERR_NO_MEM, TAG, "ota mutex alloc failed");
    s_ctx.ota.enabled = github_ota_is_configured();
    s_ctx.ota.state = OTA_STATE_IDLE;
    ota_set_status_text_locked(s_ctx.ota.enabled ? "等待检查 GitHub 最新版本" : "当前固件未启用 GitHub 自动更新");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 14;
    config.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, "httpd_start failed");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_handler, .user_ctx = NULL},
        {.uri = "/api/action/restart", .method = HTTP_POST, .handler = restart_handler, .user_ctx = NULL},
        {.uri = "/api/action/factory-reset", .method = HTTP_POST, .handler = factory_reset_handler, .user_ctx = NULL},
        {.uri = "/api/action/republish-discovery", .method = HTTP_POST, .handler = republish_handler, .user_ctx = NULL},
        {.uri = "/api/action/scd41-asc", .method = HTTP_POST, .handler = scd41_asc_handler, .user_ctx = NULL},
        {.uri = "/api/action/sps30-sleep", .method = HTTP_POST, .handler = sps30_sleep_handler, .user_ctx = NULL},
        {.uri = "/api/action/sps30-fan-cleaning", .method = HTTP_POST, .handler = sps30_fan_cleaning_handler, .user_ctx = NULL},
        {.uri = "/api/action/status-led", .method = HTTP_POST, .handler = status_led_handler, .user_ctx = NULL},
        {.uri = "/api/action/apply-frc", .method = HTTP_POST, .handler = frc_handler, .user_ctx = NULL},
        {.uri = "/api/ota/github/check", .method = HTTP_POST, .handler = github_ota_check_handler, .user_ctx = NULL},
        {.uri = "/api/ota/github/update", .method = HTTP_POST, .handler = github_ota_update_handler, .user_ctx = NULL},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler, .user_ctx = NULL},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_ctx.server, &uris[i]), TAG, "uri register failed");
    }
    return ESP_OK;
}

void provisioning_web_stop(void)
{
    if (s_ctx.server != NULL) {
        httpd_stop(s_ctx.server);
        s_ctx.server = NULL;
    }
    bool ota_running = false;
    if (s_ctx.ota.lock != NULL && xSemaphoreTake(s_ctx.ota.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ota_running = s_ctx.ota.task != NULL || s_ctx.ota.busy;
        xSemaphoreGive(s_ctx.ota.lock);
    }
    if (!ota_running) {
        if (s_ctx.ota.lock != NULL) {
            vSemaphoreDelete(s_ctx.ota.lock);
        }
        memset(&s_ctx, 0, sizeof(s_ctx));
    }
}
