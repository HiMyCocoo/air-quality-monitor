#include "provisioning_web.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_quality.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

static const char *TAG = "provisioning_web";

#define MQTT_PORT_MIN 1
#define MQTT_PORT_MAX 65535
#define MQTT_DEFAULT_PORT 1883
#define MQTT_URL_BUFFER_LEN 512
#define MQTT_SCHEME "mqtt://"
#define MAX_REQUEST_BODY_LEN 4096
#define OTA_MAX_FIRMWARE_SIZE (2 * 1024 * 1024)
#define SCD41_ALTITUDE_MIN 0
#define SCD41_ALTITUDE_MAX 3000
#define SCD41_TEMP_OFFSET_MIN 0.0
#define SCD41_TEMP_OFFSET_MAX 20.0

typedef struct {
    httpd_handle_t server;
    provisioning_web_callbacks_t callbacks;
    void *user_ctx;
    char device_id[DEVICE_ID_LEN];
} web_ctx_t;

static web_ctx_t s_ctx;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");


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

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > OTA_MAX_FIRMWARE_SIZE) {
        return send_error_json(req, "400 Bad Request", "固件大小无效或超过 2MB 限制");
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_FAIL, TAG, "no ota partition");

    esp_ota_handle_t ota_handle = 0;
    ESP_RETURN_ON_ERROR(esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle), TAG, "ota begin failed");

    char chunk[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int read = httpd_req_recv(req, chunk, remaining > (int)sizeof(chunk) ? (int)sizeof(chunk) : remaining);
        if (read <= 0) {
            esp_ota_abort(ota_handle);
            return ESP_FAIL;
        }
        esp_err_t write_err = esp_ota_write(ota_handle, chunk, read);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "ota write failed: %d", write_err);
            esp_ota_abort(ota_handle);
            return write_err;
        }
        remaining -= read;
    }

    ESP_RETURN_ON_ERROR(esp_ota_end(ota_handle), TAG, "ota end failed");
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(partition), TAG, "set boot partition failed");
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
    memset(&s_ctx, 0, sizeof(s_ctx));
    strlcpy(s_ctx.device_id, device_id, sizeof(s_ctx.device_id));
    if (callbacks != NULL) {
        s_ctx.callbacks = *callbacks;
    }
    s_ctx.user_ctx = user_ctx;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
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
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}
