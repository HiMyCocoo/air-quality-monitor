#include "platform_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_NAMESPACE "airmon_cfg"
#define CONFIG_KEY "config"
#define DEFAULT_WIFI_SSID "ong"
#define DEFAULT_WIFI_PASSWORD "jiajia990820"

static const char *TAG = "platform_config";

static void platform_config_sanitize(device_config_t *config, const char *device_id)
{
    if (config->device_name[0] == '\0' && device_id != NULL) {
        snprintf(config->device_name, sizeof(config->device_name), "aq-monitor-%.20s", device_id);
    }
    if (config->discovery_prefix[0] == '\0') {
        snprintf(config->discovery_prefix, sizeof(config->discovery_prefix), "homeassistant");
    }
    if (config->topic_root[0] == '\0' && device_id != NULL) {
        snprintf(config->topic_root, sizeof(config->topic_root), "air_quality_monitor/%s", device_id);
    }
    if (config->mqtt_port == 0) {
        config->mqtt_port = 1883;
    }
    if (config->publish_interval_sec < 5 || config->publish_interval_sec > 60) {
        config->publish_interval_sec = CONFIG_AIRMON_PUBLISH_INTERVAL_DEFAULT;
    }
    if (config->scd41_temp_offset_c < 0.0f || config->scd41_temp_offset_c > 20.0f) {
        config->scd41_temp_offset_c = 4.0f;
    }
    if (config->scd41_altitude_m > 3000) {
        config->scd41_altitude_m = 0;
    }
}

esp_err_t platform_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void platform_config_apply_defaults(device_config_t *config, const char *device_id)
{
    memset(config, 0, sizeof(*config));
    config->version = DEVICE_CONFIG_VERSION;
    strlcpy(config->wifi_ssid, DEFAULT_WIFI_SSID, sizeof(config->wifi_ssid));
    strlcpy(config->wifi_password, DEFAULT_WIFI_PASSWORD, sizeof(config->wifi_password));
    config->mqtt_port = 1883;
    config->publish_interval_sec = CONFIG_AIRMON_PUBLISH_INTERVAL_DEFAULT;
    config->scd41_asc_enabled = true;
    config->scd41_altitude_m = 0;
    config->scd41_temp_offset_c = 4.0f;
    config->pms_control_pins_enabled = true;
    platform_config_sanitize(config, device_id);
}

bool platform_config_is_complete(const device_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    return platform_config_has_wifi(config) && platform_config_has_mqtt(config);
}

bool platform_config_has_wifi(const device_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    return config->version == DEVICE_CONFIG_VERSION &&
           config->wifi_ssid[0] != '\0';
}

bool platform_config_has_mqtt(const device_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    return config->version == DEVICE_CONFIG_VERSION &&
           config->mqtt_host[0] != '\0' &&
           config->topic_root[0] != '\0' &&
           config->discovery_prefix[0] != '\0' &&
           config->mqtt_port != 0;
}

esp_err_t platform_config_load(device_config_t *config, const char *device_id)
{
    nvs_handle_t handle;
    size_t required = sizeof(*config);
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        platform_config_apply_defaults(config, device_id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open failed");

    err = nvs_get_blob(handle, CONFIG_KEY, config, &required);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || required != sizeof(*config) || config->version != DEVICE_CONFIG_VERSION) {
        ESP_LOGW(TAG, "config missing or outdated, resetting defaults");
        platform_config_apply_defaults(config, device_id);
        return ESP_ERR_NOT_FOUND;
    }

    platform_config_sanitize(config, device_id);

    return ESP_OK;
}

esp_err_t platform_config_save(const device_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open failed");

    err = nvs_set_blob(handle, CONFIG_KEY, config, sizeof(*config));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t platform_config_reset(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open failed");

    err = nvs_erase_key(handle, CONFIG_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
