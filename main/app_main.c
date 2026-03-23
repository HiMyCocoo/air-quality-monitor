#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "air_quality.h"
#include "driver/rmt_tx.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
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
#include "protocomm_ble.h"
#include "protocomm_security.h"
#include "provisioning_web.h"
#include "sensors.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "led_strip_encoder.h"

static const char *TAG = "app_main";

#define STATUS_LED_GPIO 48
#define STATUS_LED_RESOLUTION_HZ 10000000
#define STATUS_LED_BRIGHTNESS 24
#define STATUS_LED_UPDATE_MS 500

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    uint8_t last_pixels[3];
    bool initialized;
} status_led_t;

typedef struct {
    SemaphoreHandle_t lock;
    device_config_t config;
    char device_id[DEVICE_ID_LEN];
    char prov_service_name[AP_SSID_LEN];
    char prov_pop[DEVICE_ID_LEN];
    char firmware_version[FIRMWARE_VERSION_LEN];
    bool provisioning_mode;
    bool ble_provisioning_active;
    bool mqtt_started;
    bool web_started;
    bool status_led_enabled;
    bool publish_now;
    bool pending_restart;
    bool pending_factory_reset;
    int64_t action_at_ms;
    int64_t wifi_offline_since_ms;
    uint16_t frc_reference_ppm;
} app_ctx_t;

static app_ctx_t s_app;
static status_led_t s_status_led;

static bool app_status_led_is_enabled(void)
{
    bool enabled = true;
    if (s_app.lock != NULL) {
        xSemaphoreTake(s_app.lock, portMAX_DELAY);
        enabled = s_app.status_led_enabled;
        xSemaphoreGive(s_app.lock);
    }
    return enabled;
}

static uint8_t app_status_led_scale(uint8_t value)
{
    return (uint8_t)((value * STATUS_LED_BRIGHTNESS) / 255U);
}

static esp_err_t app_status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_status_led.initialized) {
        return ESP_OK;
    }

    uint8_t pixels[3] = {
        app_status_led_scale(g),
        app_status_led_scale(b),
        app_status_led_scale(r),
    };
    if (memcmp(s_status_led.last_pixels, pixels, sizeof(pixels)) == 0) {
        return ESP_OK;
    }

    memcpy(s_status_led.last_pixels, pixels, sizeof(pixels));
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_transmit(s_status_led.channel,
                                     s_status_led.encoder,
                                     pixels,
                                     sizeof(pixels),
                                     &tx_config),
                        TAG, "status led transmit failed");
    return rmt_tx_wait_all_done(s_status_led.channel, 100);
}

static esp_err_t app_status_led_init(void)
{
    memset(&s_status_led, 0, sizeof(s_status_led));

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = STATUS_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = STATUS_LED_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan_config, &s_status_led.channel),
                        TAG, "status led channel init failed");

    led_strip_encoder_config_t encoder_config = {
        .resolution = STATUS_LED_RESOLUTION_HZ,
    };
    esp_err_t err = rmt_new_led_strip_encoder(&encoder_config, &s_status_led.encoder);
    if (err != ESP_OK) {
        rmt_del_channel(s_status_led.channel);
        memset(&s_status_led, 0, sizeof(s_status_led));
        return err;
    }

    err = rmt_enable(s_status_led.channel);
    if (err != ESP_OK) {
        rmt_del_encoder(s_status_led.encoder);
        rmt_del_channel(s_status_led.channel);
        memset(&s_status_led, 0, sizeof(s_status_led));
        return err;
    }

    s_status_led.initialized = true;
    return app_status_led_set_rgb(0, 0, 0);
}

static void app_status_led_update(void)
{
    if (!app_status_led_is_enabled()) {
        app_status_led_set_rgb(0, 0, 0);
        return;
    }

    sensor_snapshot_t snapshot = {0};
    if (sensors_get_snapshot(&snapshot) != ESP_OK) {
        return;
    }

    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    air_quality_assessment_t assessment = {0};
    air_quality_compute_overall_assessment(&snapshot, &assessment);
    if (assessment.valid) {
        switch (assessment.category) {
        case AIR_QUALITY_CATEGORY_GOOD:
            green = 255;
            break;
        case AIR_QUALITY_CATEGORY_MODERATE:
            red = 255;
            green = 180;
            break;
        case AIR_QUALITY_CATEGORY_UNHEALTHY_SENSITIVE:
            red = 255;
            green = 80;
            break;
        case AIR_QUALITY_CATEGORY_UNHEALTHY:
            red = 255;
            break;
        case AIR_QUALITY_CATEGORY_VERY_UNHEALTHY:
            red = 160;
            blue = 255;
            break;
        case AIR_QUALITY_CATEGORY_HAZARDOUS:
            red = 255;
            blue = 120;
            break;
        case AIR_QUALITY_CATEGORY_UNKNOWN:
        default:
            blue = 255;
            break;
        }
        app_status_led_set_rgb(red, green, blue);
        return;
    }

    bool blink_on = ((esp_timer_get_time() / 1000 / STATUS_LED_UPDATE_MS) % 2) == 0;
    if (sensors_is_ready()) {
        blue = blink_on ? 255 : 0;
    } else {
        red = blink_on ? 255 : 0;
        blue = blink_on ? 96 : 0;
    }
    app_status_led_set_rgb(red, green, blue);
}

static void status_led_task(void *arg)
{
    (void)arg;
    while (true) {
        app_status_led_update();
        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_UPDATE_MS));
    }
}

static void app_fill_diag(device_diag_t *diag)
{
    memset(diag, 0, sizeof(*diag));
    diag->provisioning_mode = s_app.provisioning_mode;
    diag->wifi_connected = platform_wifi_is_connected();
    diag->mqtt_connected = mqtt_ha_is_connected();
    diag->sensors_ready = sensors_is_ready();
    diag->scd41_ready = sensors_is_scd41_ready();
    diag->sgp41_ready = sensors_is_sgp41_ready();
    diag->sps30_ready = sensors_is_sps30_ready();
    diag->status_led_ready = s_status_led.initialized;
    diag->status_led_enabled = s_app.status_led_enabled;
    diag->wifi_rssi = platform_wifi_get_rssi();
    diag->uptime_sec = esp_timer_get_time() / 1000000ULL;
    diag->heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    strlcpy(diag->ap_ssid, s_app.prov_service_name, sizeof(diag->ap_ssid));
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

static bool app_config_needs_restart(const device_config_t *previous, const device_config_t *updated)
{
    return strcmp(previous->wifi_ssid, updated->wifi_ssid) != 0 ||
           strcmp(previous->wifi_password, updated->wifi_password) != 0 ||
           strcmp(previous->mqtt_host, updated->mqtt_host) != 0 ||
           previous->mqtt_port != updated->mqtt_port ||
           strcmp(previous->mqtt_username, updated->mqtt_username) != 0 ||
           strcmp(previous->mqtt_password, updated->mqtt_password) != 0;
}

static bool app_config_changed_device_metadata(const device_config_t *previous, const device_config_t *updated)
{
    return strcmp(previous->device_name, updated->device_name) != 0;
}

static esp_err_t app_save_config(const device_config_t *config, bool *restart_required, bool *runtime_applied, void *user_ctx)
{
    (void)user_ctx;
    if (restart_required != NULL) {
        *restart_required = false;
    }
    if (runtime_applied != NULL) {
        *runtime_applied = false;
    }

    device_config_t previous = {0};
    device_config_t updated = *config;
    updated.version = DEVICE_CONFIG_VERSION;

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    previous = s_app.config;
    updated.scd41_asc_enabled = s_app.config.scd41_asc_enabled;
    updated.pms_control_pins_enabled = s_app.config.pms_control_pins_enabled;
    xSemaphoreGive(s_app.lock);
    platform_config_sanitize(&updated, s_app.device_id);

    bool compensation_changed =
        previous.scd41_altitude_m != updated.scd41_altitude_m ||
        fabsf(previous.scd41_temp_offset_c - updated.scd41_temp_offset_c) > 0.0001f;
    bool restart = app_config_needs_restart(&previous, &updated);
    bool metadata_changed = app_config_changed_device_metadata(&previous, &updated);

    if (compensation_changed) {
        ESP_RETURN_ON_ERROR(sensors_set_scd41_compensation(updated.scd41_altitude_m, updated.scd41_temp_offset_c),
                            TAG,
                            "scd41 compensation apply failed");
        if (runtime_applied != NULL) {
            *runtime_applied = true;
        }
    }

    esp_err_t save_err = platform_config_save(&updated);
    if (save_err != ESP_OK) {
        if (compensation_changed) {
            esp_err_t rollback_err =
                sensors_set_scd41_compensation(previous.scd41_altitude_m, previous.scd41_temp_offset_c);
            if (rollback_err != ESP_OK) {
                ESP_LOGW(TAG, "failed to roll back SCD41 compensation after config save failure: %d", rollback_err);
            }
        }
        return save_err;
    }

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.config = updated;
    if (!restart && (compensation_changed || metadata_changed)) {
        s_app.publish_now = true;
    }
    xSemaphoreGive(s_app.lock);

    if (!restart && metadata_changed && mqtt_ha_is_connected()) {
        mqtt_ha_set_device_name(updated.device_name);
        esp_err_t republish_err = mqtt_ha_publish_discovery();
        if (republish_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to republish discovery after device name update: %d", republish_err);
        }
    }

    if (restart) {
        app_schedule_action(false);
    }
    if (restart_required != NULL) {
        *restart_required = restart;
    }
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

static esp_err_t app_request_republish_web(void *user_ctx)
{
    (void)user_ctx;
    if (!mqtt_ha_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    return mqtt_ha_publish_discovery();
}

static void app_request_republish(void *user_ctx)
{
    (void)app_request_republish_web(user_ctx);
}

static esp_err_t app_request_set_scd41_asc(bool enabled, void *user_ctx)
{
    (void)user_ctx;
    esp_err_t err = sensors_set_scd41_asc(enabled);
    if (err != ESP_OK) {
        return err;
    }
    device_config_t persisted = {0};
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.config.scd41_asc_enabled = enabled;
    persisted = s_app.config;
    mqtt_ha_set_control_state(enabled, s_app.frc_reference_ppm);
    s_app.publish_now = true;
    xSemaphoreGive(s_app.lock);
    esp_err_t save_err = platform_config_save(&persisted);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist SCD41 ASC state: %d", save_err);
    }
    return ESP_OK;
}

static esp_err_t app_request_set_sps30_sleep(bool sleep, void *user_ctx)
{
    (void)user_ctx;
    esp_err_t err = sensors_set_sps30_sleep(sleep);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.publish_now = true;
    xSemaphoreGive(s_app.lock);
    return ESP_OK;
}

static esp_err_t app_request_set_status_led(bool enabled, void *user_ctx)
{
    (void)user_ctx;
    if (!s_status_led.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.status_led_enabled = enabled;
    s_app.publish_now = true;
    xSemaphoreGive(s_app.lock);

    if (!enabled) {
        return app_status_led_set_rgb(0, 0, 0);
    }
    app_status_led_update();
    return ESP_OK;
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

static esp_err_t app_start_web(void)
{
    provisioning_web_callbacks_t callbacks = {
        .get_status = app_fill_status,
        .save_config = app_save_config,
        .request_restart = app_request_restart,
        .request_factory_reset = app_request_factory_reset,
        .request_republish_discovery = app_request_republish_web,
        .request_set_scd41_asc = app_request_set_scd41_asc,
        .request_set_sps30_sleep = app_request_set_sps30_sleep,
        .request_set_status_led = app_request_set_status_led,
        .request_apply_frc = app_request_apply_frc,
    };

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    bool already_started = s_app.web_started;
    xSemaphoreGive(s_app.lock);
    if (already_started) {
        return ESP_OK;
    }

    esp_err_t err = provisioning_web_start(s_app.device_id, &callbacks, NULL);
    if (err == ESP_OK) {
        xSemaphoreTake(s_app.lock, portMAX_DELAY);
        s_app.web_started = true;
        xSemaphoreGive(s_app.lock);
    }
    return err;
}

static void app_stop_web(void)
{
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    bool was_started = s_app.web_started;
    s_app.web_started = false;
    xSemaphoreGive(s_app.lock);

    if (was_started) {
        provisioning_web_stop();
    }
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
    if (s_app.mqtt_started) {
        xSemaphoreGive(s_app.lock);
        return ESP_OK;
    }
    if (!platform_config_has_mqtt(&s_app.config)) {
        xSemaphoreGive(s_app.lock);
        return ESP_ERR_INVALID_STATE;
    }
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

static void app_build_prov_service_name(char *buffer, size_t buffer_len)
{
    snprintf(buffer, buffer_len, "%s-%s", CONFIG_AIRMON_AP_SSID_PREFIX, s_app.device_id);
    if (buffer_len > MAX_BLE_DEVNAME_LEN) {
        buffer[MAX_BLE_DEVNAME_LEN] = '\0';
    } else {
        buffer[buffer_len - 1] = '\0';
    }
}

static void app_copy_wifi_string(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    size_t copy_len = strnlen((const char *)src, src_len);
    if (copy_len >= dst_len) {
        copy_len = dst_len - 1;
    }
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static esp_err_t app_store_wifi_credentials(const wifi_sta_config_t *wifi_sta_cfg)
{
    device_config_t updated = {0};
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    updated = s_app.config;
    app_copy_wifi_string(updated.wifi_ssid, sizeof(updated.wifi_ssid), wifi_sta_cfg->ssid, sizeof(wifi_sta_cfg->ssid));
    app_copy_wifi_string(updated.wifi_password, sizeof(updated.wifi_password), wifi_sta_cfg->password, sizeof(wifi_sta_cfg->password));
    xSemaphoreGive(s_app.lock);

    esp_err_t err = platform_config_save(&updated);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.config = updated;
    xSemaphoreGive(s_app.lock);
    return ESP_OK;
}

static esp_err_t app_start_ble_provisioning(void)
{
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    bool already_active = s_app.ble_provisioning_active;
    s_app.provisioning_mode = true;
    xSemaphoreGive(s_app.lock);
    if (already_active) {
        return ESP_OK;
    }

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .wifi_prov_conn_cfg = {
            .wifi_conn_attempts = CONFIG_AIRMON_WIFI_MAX_RETRY,
        },
    };

    ESP_RETURN_ON_ERROR(platform_wifi_prepare_provisioning_sta(), TAG, "prepare provisioning STA failed");
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(config), TAG, "wifi_prov_mgr_init failed");
    ESP_LOGW(TAG, "Starting BLE provisioning. Service name: %s, PoP: %s", s_app.prov_service_name, s_app.prov_pop);

    esp_err_t err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                                     (const void *)s_app.prov_pop,
                                                     s_app.prov_service_name,
                                                     NULL);
    if (err != ESP_OK) {
        wifi_prov_mgr_deinit();
        xSemaphoreTake(s_app.lock, portMAX_DELAY);
        s_app.provisioning_mode = false;
        xSemaphoreGive(s_app.lock);
        return err;
    }

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    s_app.ble_provisioning_active = true;
    s_app.wifi_offline_since_ms = 0;
    xSemaphoreGive(s_app.lock);
    return ESP_OK;
}

static void app_stop_ble_provisioning(void)
{
    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    bool active = s_app.ble_provisioning_active;
    xSemaphoreGive(s_app.lock);

    if (active) {
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_wait();
    }
}

static void app_prov_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "BLE provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                char ssid[WIFI_SSID_LEN + 1] = {0};
                app_copy_wifi_string(ssid, sizeof(ssid), wifi_sta_cfg->ssid, sizeof(wifi_sta_cfg->ssid));
                ESP_LOGI(TAG, "Provisioning received Wi-Fi SSID: %s", ssid);
                if (app_store_wifi_credentials(wifi_sta_cfg) != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to persist provisioned Wi-Fi credentials");
                }
                break;
            }
            case WIFI_PROV_CRED_FAIL:
                ESP_LOGW(TAG, "BLE provisioning Wi-Fi connect failed");
                break;
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "BLE provisioning Wi-Fi connect succeeded");
                break;
            case WIFI_PROV_END:
                ESP_LOGI(TAG, "BLE provisioning stopped");
                xSemaphoreTake(s_app.lock, portMAX_DELAY);
                s_app.ble_provisioning_active = false;
                s_app.provisioning_mode = false;
                xSemaphoreGive(s_app.lock);
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
        return;
    }

    if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
            ESP_LOGI(TAG, "Provisioning BLE client connected");
        } else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
            ESP_LOGI(TAG, "Provisioning BLE client disconnected");
        }
        return;
    }

    if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (event_id == PROTOCOMM_SECURITY_SESSION_SETUP_OK) {
            ESP_LOGI(TAG, "Provisioning secure session established");
        } else if (event_id == PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS) {
            ESP_LOGW(TAG, "Provisioning secure session got invalid params");
        } else if (event_id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH) {
            ESP_LOGW(TAG, "Provisioning PoP mismatch");
        }
    }
}

static void app_wifi_event(bool connected, void *user_ctx)
{
    (void)user_ctx;
    bool start_web = false;
    bool start_mqtt = false;

    xSemaphoreTake(s_app.lock, portMAX_DELAY);
    if (connected) {
        s_app.wifi_offline_since_ms = 0;
        s_app.publish_now = true;
        s_app.provisioning_mode = false;
        start_web = !s_app.web_started;
        start_mqtt = !s_app.mqtt_started && platform_config_has_mqtt(&s_app.config);
    }
    xSemaphoreGive(s_app.lock);

    if (connected) {
        if (start_web) {
            app_start_web();
        }
        if (start_mqtt) {
            app_start_mqtt();
        }
    }
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
                    ESP_LOGW(TAG, "Wi-Fi offline too long, switching back to BLE provisioning");
                    app_stop_mqtt();
                    app_stop_web();
                    app_stop_ble_provisioning();
                    app_start_ble_provisioning();
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
    s_app.status_led_enabled = true;

    app_build_device_id(s_app.device_id, sizeof(s_app.device_id));
    app_build_prov_service_name(s_app.prov_service_name, sizeof(s_app.prov_service_name));
    strlcpy(s_app.prov_pop, s_app.device_id, sizeof(s_app.prov_pop));
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strlcpy(s_app.firmware_version, app_desc->version, sizeof(s_app.firmware_version));

    ESP_ERROR_CHECK(platform_config_init());
    platform_config_load(&s_app.config, s_app.device_id);
    ESP_ERROR_CHECK(platform_wifi_init(app_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &app_prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &app_prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &app_prov_event_handler, NULL));

    ESP_ERROR_CHECK(sensors_start(&s_app.config));
    esp_err_t led_err = app_status_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "status led init failed on GPIO%d: %d", STATUS_LED_GPIO, led_err);
    }

    bool use_ble_provisioning = !platform_config_has_wifi(&s_app.config);
    if (!use_ble_provisioning) {
        esp_err_t sta_err = platform_wifi_start_sta(&s_app.config,
                                                    CONFIG_AIRMON_WIFI_MAX_RETRY,
                                                    CONFIG_AIRMON_WIFI_CONNECT_TIMEOUT_MS);
        use_ble_provisioning = sta_err != ESP_OK;
    }

    if (use_ble_provisioning) {
        ESP_ERROR_CHECK(app_start_ble_provisioning());
        s_app.provisioning_mode = true;
    } else {
        s_app.provisioning_mode = false;
        ESP_ERROR_CHECK(app_start_web());
        if (platform_config_has_mqtt(&s_app.config)) {
            ESP_ERROR_CHECK(app_start_mqtt());
        } else {
            ESP_LOGW(TAG, "Wi-Fi connected but MQTT host is not configured yet; use the web console to finish setup");
        }
    }

    xTaskCreate(publisher_task, "publisher_task", 6144, NULL, 4, NULL);
    xTaskCreate(supervisor_task, "supervisor_task", 6144, NULL, 4, NULL);
    if (s_status_led.initialized) {
        xTaskCreate(status_led_task, "status_led_task", 4096, NULL, 3, NULL);
    }
}
