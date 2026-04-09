#include "ota_manager_internal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_AIRMON_GITHUB_AUTH_TOKEN
#define CONFIG_AIRMON_GITHUB_AUTH_TOKEN ""
#endif

static const char *TAG = OTA_MANAGER_TAG;

typedef struct {
    int major;
    int minor;
    int patch;
} version_triplet_t;

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} http_buffer_t;

bool ota_manager_github_is_configured(void)
{
#if CONFIG_AIRMON_GITHUB_OTA_ENABLED
    return CONFIG_AIRMON_GITHUB_RELEASE_OWNER[0] != '\0' &&
           CONFIG_AIRMON_GITHUB_RELEASE_REPO[0] != '\0' &&
           CONFIG_AIRMON_GITHUB_OTA_ASSET_PREFIX[0] != '\0';
#else
    return false;
#endif
}

const char *ota_manager_github_repo_name(void)
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

int ota_manager_compare_versions(const char *left, const char *right)
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
                 ota_manager_github_repo_name());
    } else {
        snprintf(error,
                 error_len,
                 "GitHub 仓库 %s 不存在或不可匿名访问。若这是私有仓库，请在固件配置中提供 GitHub Token",
                 ota_manager_github_repo_name());
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
                 ota_manager_github_repo_name());
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

esp_err_t ota_manager_github_fetch_latest_release(ota_release_info_t *info, char *error, size_t error_len)
{
    if (info == NULL) {
        snprintf(error, error_len, "GitHub Release 目标缓冲区为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota_manager_github_is_configured()) {
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

esp_err_t ota_manager_github_download_and_apply_ota(const ota_release_info_t *info, char *error, size_t error_len)
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
        ota_manager_report_progress(image_len_read > 0 ? (size_t)image_len_read : 0,
                                    image_size > 0 ? (size_t)image_size : 0,
                                    "正在从 GitHub 下载并写入固件...");
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
