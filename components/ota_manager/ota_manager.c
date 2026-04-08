#include "ota_manager.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_types.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_AIRMON_GITHUB_AUTH_TOKEN
#define CONFIG_AIRMON_GITHUB_AUTH_TOKEN ""
#endif

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

static const char *TAG = "ota_manager";

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
    int major;
    int minor;
    int patch;
} version_triplet_t;

typedef struct {
    char version[FIRMWARE_VERSION_LEN];
    char tag[OTA_GITHUB_TAG_LEN];
    char asset_name[OTA_GITHUB_ASSET_NAME_LEN];
    char asset_url[OTA_GITHUB_URL_LEN];
    char asset_api_url[OTA_GITHUB_URL_LEN];
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

static bool github_auth_token_configured(void)
{
#if CONFIG_AIRMON_GITHUB_OTA_ENABLED
    return CONFIG_AIRMON_GITHUB_AUTH_TOKEN[0] != '\0';
#else
    return false;
#endif
}

static void github_apply_auth_header(esp_http_client_handle_t client)
{
#if CONFIG_AIRMON_GITHUB_OTA_ENABLED
    if (client == NULL || !github_auth_token_configured()) {
        return;
    }

    char auth_header[OTA_GITHUB_AUTH_HEADER_LEN];
    int written = snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_AIRMON_GITHUB_AUTH_TOKEN);
    if (written <= 0 || written >= (int)sizeof(auth_header)) {
        ESP_LOGW(TAG, "github auth token is too long, Authorization header skipped");
        return;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);
#else
    (void)client;
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

static esp_err_t github_http_get(const char *url,
                                 const char *accept,
                                 bool api_request,
                                 http_buffer_t *response,
                                 int *status_code,
                                 char *error,
                                 size_t error_len)
{
    if (url == NULL || response == NULL) {
        snprintf(error, error_len, "GitHub HTTP 请求参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = OTA_HTTP_USER_AGENT,
        .event_handler = http_collect_event_handler,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(error, error_len, "GitHub HTTP 客户端初始化失败");
        return ESP_ERR_NO_MEM;
    }

    if (accept != NULL && accept[0] != '\0') {
        esp_http_client_set_header(client, "Accept", accept);
    }
    if (api_request) {
        esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");
    }
    github_apply_auth_header(client);

    esp_err_t err = esp_http_client_perform(client);
    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    if (err != ESP_OK) {
        snprintf(error, error_len, "GitHub 请求失败: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

static void github_describe_repo_access_error(char *error, size_t error_len)
{
    if (github_auth_token_configured()) {
        snprintf(error,
                 error_len,
                 "GitHub 仓库 %s 不存在，或当前 Token 无权访问该仓库",
                 github_repo_name());
    } else {
        snprintf(error,
                 error_len,
                 "GitHub 仓库 %s 不存在或不可匿名访问。若这是私有仓库，请在固件配置中提供 GitHub Token",
                 github_repo_name());
    }
}

static esp_err_t github_probe_repo_visibility(bool *visible, char *error, size_t error_len)
{
    if (visible == NULL) {
        snprintf(error, error_len, "GitHub 仓库探测参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    char api_url[OTA_GITHUB_URL_LEN];
    snprintf(api_url,
             sizeof(api_url),
             "https://api.github.com/repos/%s/%s",
             CONFIG_AIRMON_GITHUB_RELEASE_OWNER,
             CONFIG_AIRMON_GITHUB_RELEASE_REPO);

    http_buffer_t response = {0};
    int status_code = 0;
    esp_err_t err = github_http_get(api_url,
                                    "application/vnd.github+json",
                                    true,
                                    &response,
                                    &status_code,
                                    error,
                                    error_len);
    if (err != ESP_OK) {
        http_buffer_free(&response);
        return err;
    }

    *visible = status_code >= 200 && status_code < 300;
    if (!*visible && status_code != 404) {
        if (!github_json_message(response.data, error, error_len)) {
            snprintf(error, error_len, "GitHub 仓库探测返回 HTTP %d", status_code);
        }
        http_buffer_free(&response);
        return ESP_FAIL;
    }

    http_buffer_free(&response);
    return ESP_OK;
}

static void github_describe_latest_release_not_found(char *error, size_t error_len)
{
    bool repo_visible = false;
    char probe_error[OTA_ERROR_TEXT_LEN] = {0};
    esp_err_t probe_err = github_probe_repo_visibility(&repo_visible, probe_error, sizeof(probe_error));
    if (probe_err == ESP_OK && repo_visible) {
        snprintf(error,
                 error_len,
                 "GitHub 仓库 %s 可访问，但还没有已发布的正式 Release（草稿或预发布不算 latest）",
                 github_repo_name());
        return;
    }
    if (probe_err == ESP_OK) {
        github_describe_repo_access_error(error, error_len);
        return;
    }
    if (probe_error[0] != '\0') {
        strlcpy(error, probe_error, error_len);
        return;
    }
    snprintf(error, error_len, "GitHub 最新 Release 不存在");
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
    cJSON *asset_api_url = cJSON_GetObjectItemCaseSensitive(selected, "url");
    cJSON *asset_url = cJSON_GetObjectItemCaseSensitive(selected, "browser_download_url");
    strlcpy(info->asset_name, asset_name->valuestring, sizeof(info->asset_name));
    if (cJSON_IsString(asset_api_url)) {
        strlcpy(info->asset_api_url, asset_api_url->valuestring, sizeof(info->asset_api_url));
    }
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
        return ESP_ERR_NOT_SUPPORTED;
    }

    char api_url[OTA_GITHUB_URL_LEN];
    snprintf(api_url,
             sizeof(api_url),
             "https://api.github.com/repos/%s/%s/releases/latest",
             CONFIG_AIRMON_GITHUB_RELEASE_OWNER,
             CONFIG_AIRMON_GITHUB_RELEASE_REPO);

    http_buffer_t response = {0};
    int status_code = 0;
    esp_err_t err = github_http_get(api_url,
                                    "application/vnd.github+json",
                                    true,
                                    &response,
                                    &status_code,
                                    error,
                                    error_len);
    if (err != ESP_OK) {
        http_buffer_free(&response);
        return err;
    }
    if (status_code < 200 || status_code >= 300) {
        if (status_code == 404) {
            github_describe_latest_release_not_found(error, error_len);
        } else if (!github_json_message(response.data, error, error_len)) {
            snprintf(error, error_len, "GitHub Release 查询返回 HTTP %d", status_code);
        }
        http_buffer_free(&response);
        return ESP_FAIL;
    }

    memset(info, 0, sizeof(*info));
    bool parsed = github_parse_release_info(response.data, info, error, error_len);
    http_buffer_free(&response);
    return parsed ? ESP_OK : ESP_FAIL;
}

static esp_err_t github_ota_http_client_init(esp_http_client_handle_t client)
{
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_ARG, TAG, "ota http client null");
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    if (github_auth_token_configured()) {
        esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");
        github_apply_auth_header(client);
    }
    return ESP_OK;
}

static esp_err_t github_download_and_apply_ota(const ota_release_info_t *info, char *error, size_t error_len)
{
    const char *download_url = NULL;
    if (info != NULL) {
        if (github_auth_token_configured() && info->asset_api_url[0] != '\0') {
            download_url = info->asset_api_url;
        } else if (info->asset_url[0] != '\0') {
            download_url = info->asset_url;
        } else if (info->asset_api_url[0] != '\0') {
            download_url = info->asset_api_url;
        }
    }
    if (download_url == NULL || download_url[0] == '\0') {
        snprintf(error, error_len, "GitHub OTA 下载地址为空");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t http_config = {
        .url = download_url,
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
        s_ctx.callbacks.request_restart(s_ctx.callbacks.user_ctx);
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
    s_ctx.enabled = github_ota_is_configured();
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
    snapshot.enabled = github_ota_is_configured();
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

    return ESP_OK;
}

esp_err_t ota_manager_check_github_release(char *error, size_t error_len)
{
    if (!github_ota_is_configured()) {
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
    esp_err_t err = github_fetch_latest_release(&info, error, error_len);
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_version = app_desc != NULL ? app_desc->version : "";

    if (!ota_lock_take(pdMS_TO_TICKS(1000))) {
        snprintf(error, error_len, "OTA 状态锁不可用");
        return ESP_ERR_TIMEOUT;
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

    return err;
}

esp_err_t ota_manager_start_github_update(char *error, size_t error_len)
{
    if (!github_ota_is_configured()) {
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
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_finish_error_locked("无法启动 GitHub OTA 后台任务");
            ota_lock_give();
        }
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

esp_err_t ota_manager_upload_firmware(httpd_req_t *req, char *error, size_t error_len)
{
    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "ota request null");

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        snprintf(error, error_len, "未找到 OTA 分区");
        return ESP_ERR_NOT_FOUND;
    }
    if (req->content_len <= 0) {
        snprintf(error, error_len, "固件文件为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len > OTA_MAX_FIRMWARE_SIZE || (size_t)req->content_len > partition->size) {
        snprintf(error, error_len, "固件大小无效或超过 OTA 分区限制");
        return ESP_ERR_INVALID_ARG;
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
    ota_prepare_locked(OTA_STATE_UPLOAD, OTA_SOURCE_UPLOAD, "正在上传本地固件...");
    s_ctx.total_bytes = (size_t)req->content_len;
    ota_lock_give();

    esp_ota_handle_t ota_handle = 0;
    esp_err_t begin_err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (begin_err != ESP_OK) {
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_finish_error_locked("OTA 分区初始化失败");
            ota_lock_give();
        }
        snprintf(error, error_len, "OTA 分区初始化失败");
        return begin_err;
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
            snprintf(error, error_len, "固件上传被中断，请重试");
            return ESP_FAIL;
        }
        esp_err_t write_err = esp_ota_write(ota_handle, chunk, read);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "ota write failed: %d", write_err);
            esp_ota_abort(ota_handle);
            if (ota_lock_take(pdMS_TO_TICKS(1000))) {
                ota_finish_error_locked("固件写入失败，请重试");
                ota_lock_give();
            }
            snprintf(error, error_len, "固件写入失败，请重试");
            return write_err;
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
        snprintf(error, error_len, "固件校验失败，请确认上传的是 OTA 应用固件");
        return ESP_ERR_INVALID_ARG;
    }

    esp_app_desc_t new_desc = {0};
    esp_err_t desc_err = esp_ota_get_partition_description(partition, &new_desc);
    if (desc_err == ESP_OK) {
        const esp_app_desc_t *running_desc = esp_app_get_description();
        if (running_desc != NULL &&
            strncmp(new_desc.project_name, running_desc->project_name, sizeof(new_desc.project_name)) != 0) {
            if (ota_lock_take(pdMS_TO_TICKS(1000))) {
                ota_finish_error_locked("固件项目名称不匹配，拒绝刷入非本项目固件");
                ota_lock_give();
            }
            snprintf(error, error_len, "固件项目名称不匹配，拒绝刷入非本项目固件");
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t boot_err = esp_ota_set_boot_partition(partition);
    if (boot_err != ESP_OK) {
        if (ota_lock_take(pdMS_TO_TICKS(1000))) {
            ota_finish_error_locked("切换启动分区失败");
            ota_lock_give();
        }
        snprintf(error, error_len, "切换启动分区失败");
        return boot_err;
    }

    if (ota_lock_take(pdMS_TO_TICKS(1000))) {
        ota_finish_success_locked("本地固件上传完成，设备即将重启");
        ota_lock_give();
    }
    if (s_ctx.callbacks.request_restart != NULL) {
        s_ctx.callbacks.request_restart(s_ctx.callbacks.user_ctx);
    }
    return ESP_OK;
}
