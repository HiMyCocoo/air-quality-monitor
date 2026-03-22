#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_ha.h"
#include "platform_config.h"
#include "platform_wifi.h"
#include "provisioning_web.h"
#include "sensors.h"

static const char *TAG = "app_main";

typedef struct {
    SemaphoreHandle_t lock;
    device_config_t config;
    char device_id[DEVICE_ID_LEN];
    char ap_ssid[AP_SSID_LEN];
    char firmware_version[FIRMWARE_VERSION_LEN];
    bool provisioning_mode;
    bool mqtt_started;
    bool publish_now;
    bool pending_restart;
    bool pending_factory_reset;
    int64_t action_at_ms;
    int64_t wifi_offline_since_ms;
    uint16_t frc_reference_ppm;
} app_ctx_t;

static app_ctx_t s_app;

static void app_fill_diag(device_diag_t *diag)
{
    memset(diag, 0, sizeof(*diag));
    diag->provisioning_mode = s_app.provisioning_mode;
    diag->wifi_connected = platform_wifi_is_connected();
    diag->mqtt_connected = mqtt_ha_is_connected();
    diag->sensors_ready = sensors_is_ready();
    diag->wifi_rssi = platform_wifi_get_rssi();
    diag->uptime_sec = esp_timer_get_time() / 1000000ULL;
    diag->heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    strlcpy(diag->ap_ssid, s_app.ap_ssid, sizeof(diag->ap_ssid));
    strlcpy(diag->device_id, s_app.device_id, sizeof(diag->device_id));
    strlcpy(diag->firmware_version, s_app.firmware_version, sizeof(diag->firmware_version));
    platform_wifi_get_ip(diag->ip_addr, sizeof(diag->ip_addr));
    sensors_get_last_error(diag->last_error, sizeof(diag->last_error));
}

static void app_fill_status(sensor_snapshot_t *snapshot, device_diag_t *diag, device_config_t *config, uint16_t *frc_ppm, void *user_ctx)
{
    (void)user_ctx;
    memset(snapshot, 0, sizeof(*snapshot));
    sensors_get_snapshot(snapshot);

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    *config = s_app.config;
    *frc_ppm = s_app.frc_reference_ppm;
    app_fill_diag(diag);
    xSemaphoreGive(s_app.lock);
}

static void app_schedule_action(bool factory_reset)
{
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.pending_restart = !factory_reset;
    s_app.pending_factory_reset = factory_reset;
    s_app.action_at_ms = (esp_timer_get_time() / 1000) + 1500;
    xSemaphoreGive(s_app.lock);
}

static esp_err_t app_save_config(const device_config_t *config, void *user_ctx)
{
    (void)user_ctx;
    device_config_t updated = *config;
    updated.version = DEVICE_CONFIG_VERSION;
    if (updated.device_name[0] == '\0') {
        snprintf(updated.device_name, sizeof(updated.device_name), "aq-monitor-%.20s", s_app.device_id);
    }
    if (updated.discovery_prefix[0] == '\0') {
        strlcpy(updated.discovery_prefix, "homeassistant", sizeof(updated.discovery_prefix));
    }
    if (updated.topic_root[0] == '\0') {
        snprintf(updated.topic_root, sizeof(updated.topic_root), "air_quality_monitor/%s", s_app.device_id);
    }
    if (updated.publish_interval_sec < 5 || updated.publish_interval_sec > 60) {
        updated.publish_interval_sec = CONFIG_AIRMON_PUBLISH_INTERVAL_DEFAULT;
    }
    if (updated.mqtt_port == 0) {
        updated.mqtt_port = 1883;
    }
    if (updated.scd41_altitude_m > 3000) {
        updated.scd41_altitude_m = 0;
    }
    if (updated.scd41_temp_offset_c < 0.0f || updated.scd41_temp_offset_c > 20.0f) {
        updated.scd41_temp_offset_c = 4.0f;
    }

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    updated.scd41_asc_enabled = s_app.config.scd41_asc_enabled;
    updated.pms_control_pins_enabled = s_app.config.pms_control_pins_enabled;
    s_app.config = updated;
    xSemaphoreGive(s_app.lock);

    ESP_RETURN_ON_ERROR(platform_config_save(&updated), TAG, "config save failed");
    app_schedule_action(false);
    return ESP_OK;
}

static void app_request_restart(void *user_ctx)
{
    (void)user_ctx;
    app_schedule_action(false);
}

static void app_request_factory_reset(void *user_ctx)
{
    (void)user_ctx;
    app_schedule_action(true);
}

static void app_request_republish(void *user_ctx)
{
    (void)user_ctx;
    if (mqtt_ha_is_connected()) {
        mqtt_ha_publish_discovery();
    }
}

static void app_request_set_scd41_asc(bool enabled, void *user_ctx)
{
    (void)user_ctx;
    if (sensors_set_scd41_asc(enabled) == ESP_OK) {
        xSemaphoreTake(s_app.lock, portMAX_DELAY);
        s_app.config.scd41_asc_enabled = enabled;
        mqtt_ha_set_control_state(enabled, s_app.frc_reference_ppm);
        s_app.publish_now = true;
        platform_config_save(&s_app.config);
        xSemaphoreGive(s_app.lock);
    }
}

static void app_request_set_sps30_sleep(bool sleep, void *user_ctx)
{
    (void)user_ctx;
    if (sensors_set_sps30_sleep(sleep) == ESP_OK) {
        xSemaphoreTake(s_app.lock, portMAX_DELAY);
        s_app.publish_now = true;
        xSemaphoreGive(s_app.lock);
    }
}

static esp_err_t app_request_apply_frc(uint16_t ppm, void *user_ctx)
{
    (void)user_ctx;
    esp_err_t err = sensors_set_scd41_forced_recalibration(ppm);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.frc_reference_ppm = ppm;
    mqtt_ha_set_control_state(s_app.config.scd41_asc_enabled, s_app.frc_reference_ppm);
    s_app.publish_now = true;
    xSemaphoreGive(s_app.lock);
    return ESP_OK;
}

static void app_mqtt_connected(void *user_ctx)
{
    (void)user_ctx;
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.publish_now = true;
    xSemaphoreGive(s_app.lock);
}

static void app_wifi_event(bool connected, void *user_ctx)
{
    (void)user_ctx;
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    if (connected) {
        s_app.wifi_offline_since_ms = 0;
        s_app.publish_now = true;
    }
    xSemaphoreGive(s_app.lock);
}

static esp_err_t app_start_web(void)
{
    provisioning_web_callbacks_t callbacks = {
        .get_status = app_fill_status,
        .save_config = app_save_config,
        .request_restart = app_request_restart,
        .request_factory_reset = app_request_factory_reset,
        .request_republish_discovery = app_request_republish,
        .request_set_scd41_asc = app_request_set_scd41_asc,
        .request_set_sps30_sleep = app_request_set_sps30_sleep,
        .request_apply_frc = app_request_apply_frc,
    };
    return provisioning_web_start(s_app.device_id, &callbacks, NULL);
}

static esp_err_t app_start_mqtt(void)
{
    mqtt_ha_callbacks_t callbacks = {
        .restart_requested = app_request_restart,
        .factory_reset_requested = app_request_factory_reset,
        .republish_requested = app_request_republish,
        .set_scd41_asc_requested = app_request_set_scd41_asc,
        .set_sps30_sleep_requested = app_request_set_sps30_sleep,
        .apply_scd41_frc_requested = app_request_apply_frc,
        .connected = app_mqtt_connected,
    };

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    esp_err_t err = mqtt_ha_start(&s_app.config, s_app.device_id, s_app.firmware_version, &callbacks, NULL);
    if (err == ESP_OK) {
        s_app.mqtt_started = true;
        mqtt_ha_set_control_state(s_app.config.scd41_asc_enabled, s_app.frc_reference_ppm);
    }
    xSemaphoreGive(s_app.lock);
    return err;
}

static void app_stop_mqtt(void)
{
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    if (s_app.mqtt_started) {
        mqtt_ha_stop();
        s_app.mqtt_started = false;
    }
    xSemaphoreGive(s_app.lock);
}

static void app_build_device_id(char *buffer, size_t buffer_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buffer, buffer_len, "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void publisher_task(void *arg)
{
    (void)arg;
    int64_t last_publish_ms = 0;

    while (true) {
        bool should_publish = false;
        uint16_t interval_sec = CONFIG_AIRMON_PUBLISH_INTERVAL_DEFAULT;

        xSemaphoreTake(s_app.lock, portMAX_DELAY);
        interval_sec = s_app.config.publish_interval_sec ? s_app.config.publish_interval_sec : CONFIG_AIRMON_PUBLISH_INTERVAL_DEFAULT;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_app.publish_now || (now_ms - last_publish_ms) >= (interval_sec * 1000LL)) {
            should_publish = true;
            s_app.publish_now = false;
        }
        xSemaphoreGive(s_app.lock);

        if (should_publish && mqtt_ha_is_connected()) {
            sensor_snapshot_t snapshot = {0};
            device_diag_t diag = {0};
            sensors_get_snapshot(&snapshot);
            xSemaphoreTake(s_app.lock, portMAX_DELAY);
            app_fill_diag(&diag);
            mqtt_ha_set_control_state(s_app.config.scd41_asc_enabled, s_app.frc_reference_ppm);
            xSemaphoreGive(s_app.lock);
            mqtt_ha_publish_state(&snapshot, &diag);
            last_publish_ms = esp_timer_get_time() / 1000;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void supervisor_task(void *arg)
{
    (void)arg;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        xSemaphoreTake(s_app.lock, portMAX_DELAY);
        if (s_app.pending_factory_reset && now_ms >= s_app.action_at_ms) {
            xSemaphoreGive(s_app.lock);
            platform_config_reset();
            esp_restart();
        }
        if (s_app.pending_restart && now_ms >= s_app.action_at_ms) {
            xSemaphoreGive(s_app.lock);
            esp_restart();
        }

        if (!s_app.provisioning_mode) {
            if (platform_wifi_is_connected()) {
                s_app.wifi_offline_since_ms = 0;
                if (!s_app.mqtt_started) {
                    xSemaphoreGive(s_app.lock);
                    app_start_mqtt();
                    xSemaphoreTake(s_app.lock, portMAX_DELAY);
                }
            } else {
                if (s_app.wifi_offline_since_ms == 0) {
                    s_app.wifi_offline_since_ms = now_ms;
                } else if ((now_ms - s_app.wifi_offline_since_ms) > (CONFIG_AIRMON_AP_FALLBACK_TIMEOUT_SEC * 1000LL)) {
                    xSemaphoreGive(s_app.lock);
                    ESP_LOGW(TAG, "Wi-Fi offline too long, switching back to AP mode");
                    app_stop_mqtt();
                    provisioning_web_stop();
                    platform_wifi_switch_to_ap(s_app.ap_ssid);
                    app_start_web();
                    xSemaphoreTake(s_app.lock, portMAX_DELAY);
                    s_app.provisioning_mode = true;
                    s_app.wifi_offline_since_ms = 0;
                }
            }
        }
        xSemaphoreGive(s_app.lock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    memset(&s_app, 0, sizeof(s_app));
    s_app.lock = xSemaphoreCreateMutex();
    if (s_app.lock == NULL) {
        ESP_LOGE(TAG, "failed to create app mutex");
        return;
    }
    s_app.frc_reference_ppm = 400;

    app_build_device_id(s_app.device_id, sizeof(s_app.device_id));
    platform_wifi_build_ap_ssid(s_app.ap_ssid, sizeof(s_app.ap_ssid));
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strlcpy(s_app.firmware_version, app_desc->version, sizeof(s_app.firmware_version));

    ESP_ERROR_CHECK(platform_config_init());
    platform_config_load(&s_app.config, s_app.device_id);
    ESP_ERROR_CHECK(platform_wifi_init(app_wifi_event, NULL));

    ESP_ERROR_CHECK(sensors_start(&s_app.config));

    bool use_ap = !platform_config_is_complete(&s_app.config);
    if (!use_ap) {
        esp_err_t sta_err = platform_wifi_start_sta(&s_app.config,
                                                    CONFIG_AIRMON_WIFI_MAX_RETRY,
                                                    CONFIG_AIRMON_WIFI_CONNECT_TIMEOUT_MS);
        use_ap = sta_err != ESP_OK;
    }

    if (use_ap) {
        ESP_ERROR_CHECK(platform_wifi_start_ap(s_app.ap_ssid));
        s_app.provisioning_mode = true;
    } else {
        s_app.provisioning_mode = false;
        app_start_mqtt();
    }

    ESP_ERROR_CHECK(app_start_web());

    xTaskCreate(publisher_task, "publisher_task", 6144, NULL, 4, NULL);
    xTaskCreate(supervisor_task, "supervisor_task", 6144, NULL, 4, NULL);
}
