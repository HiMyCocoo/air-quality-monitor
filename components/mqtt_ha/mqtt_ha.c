#include "mqtt_ha.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "air_quality.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt_ha";

typedef struct {
    const char *object_id;
    const char *name;
    const char *json_key;
    const char *unit;
    const char *device_class;
    const char *state_class;
    const char *entity_category;
    bool diagnostic;
} sensor_entity_t;

typedef struct {
    const char *object_id;
    const char *name;
    const char *json_key;
    const char *device_class;
    const char *entity_category;
    bool diagnostic;
} binary_sensor_entity_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    _Atomic bool connected;
    _Atomic bool scd41_asc_enabled;
    _Atomic uint16_t frc_reference_ppm;
    device_config_t config;
    mqtt_ha_callbacks_t callbacks;
    void *user_ctx;
    char device_id[DEVICE_ID_LEN];
    char firmware_version[FIRMWARE_VERSION_LEN];
    char state_topic[TOPIC_ROOT_LEN + 32];
    char diag_topic[TOPIC_ROOT_LEN + 32];
    char availability_topic[TOPIC_ROOT_LEN + 32];
    char cmd_prefix[TOPIC_ROOT_LEN + 32];
    char broker_uri[128];
    char last_error[LAST_ERROR_LEN];
} mqtt_ctx_t;

static mqtt_ctx_t s_ctx;
static portMUX_TYPE s_error_mux = portMUX_INITIALIZER_UNLOCKED;

static void mqtt_set_last_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    portENTER_CRITICAL(&s_error_mux);
    vsnprintf(s_ctx.last_error, sizeof(s_ctx.last_error), fmt, args);
    portEXIT_CRITICAL(&s_error_mux);
    va_end(args);
}

static void mqtt_clear_last_error(void)
{
    portENTER_CRITICAL(&s_error_mux);
    s_ctx.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_error_mux);
}

static const char *mqtt_connect_return_code_text(int code)
{
    switch (code) {
    case 0x01:
        return "unacceptable protocol version";
    case 0x02:
        return "client id rejected";
    case 0x03:
        return "server unavailable";
    case 0x04:
        return "bad username or password";
    case 0x05:
        return "not authorized";
    default:
        return "unknown";
    }
}


static const sensor_entity_t SENSOR_ENTITIES[] = {
    {"co2", "CO2", "co2", "ppm", "carbon_dioxide", "measurement", NULL, false},
    {"co2_rating", "CO2 Ventilation Status", "co2_rating", NULL, NULL, NULL, NULL, false},
    {"co2_compensation_source", "CO2 Compensation Source", "co2_compensation_source", NULL, NULL, NULL, "diagnostic", true},
    {"temperature", "Temperature", "temperature", "°C", "temperature", "measurement", NULL, false},
    {"temperature_rating", "Temperature Rating", "temperature_rating", NULL, NULL, NULL, NULL, false},
    {"humidity", "Humidity", "humidity", "%", "humidity", "measurement", NULL, false},
    {"humidity_rating", "Humidity Rating", "humidity_rating", NULL, NULL, NULL, NULL, false},
    {"humidity_trend_3h", "Humidity Trend (3h Equivalent)", "humidity_trend_rh_3h", "%", "humidity", "measurement", NULL, false},
    {"bmp390_temperature", "BMP390 Temperature", "bmp390_temperature", "°C", "temperature", "measurement", NULL, false},
    {"pressure", "Pressure", "pressure", "hPa", "atmospheric_pressure", "measurement", NULL, false},
    {"pressure_trend_3h", "Pressure Trend (3h Equivalent)", "pressure_trend_hpa_3h", "hPa", NULL, "measurement", NULL, false},
    {"pressure_trend", "Pressure Trend", "pressure_trend", NULL, NULL, NULL, NULL, false},
    {"dew_point_spread", "Dew Point Spread", "dew_point_spread_c", "°C", "temperature", "measurement", NULL, false},
    {"rain_outlook", "Rain Outlook", "rain_outlook", NULL, NULL, NULL, NULL, false},
    {"rain_season", "Rain Season Context", "rain_season", NULL, NULL, NULL, NULL, false},
    {"voc_index", "VOC Index (Sensirion)", "voc_index", NULL, NULL, "measurement", NULL, false},
    {"voc_rating", "VOC Event Level (Sensirion)", "voc_rating", NULL, NULL, NULL, NULL, false},
    {"nox_index", "NOx Index (Sensirion)", "nox_index", NULL, NULL, "measurement", NULL, false},
    {"nox_rating", "NOx Event Level (Sensirion)", "nox_rating", NULL, NULL, NULL, NULL, false},
    {"sgp41_voc_stabilization_remaining_s",
     "SGP41 VOC Stabilization Remaining",
     "sgp41_voc_stabilization_remaining_s",
     "s",
     "duration",
     "measurement",
     "diagnostic",
     true},
    {"sgp41_nox_stabilization_remaining_s",
     "SGP41 NOx Stabilization Remaining",
     "sgp41_nox_stabilization_remaining_s",
     "s",
     "duration",
     "measurement",
     "diagnostic",
     true},
    {"pm1_0", "PM1.0", "pm1_0", "µg/m³", "pm1", "measurement", NULL, false},
    {"pm2_5", "PM2.5", "pm2_5", "µg/m³", "pm25", "measurement", NULL, false},
    {"pm4_0", "PM4.0", "pm4_0", "µg/m³", NULL, "measurement", NULL, false},
    {"pm10_0", "PM10", "pm10_0", "µg/m³", "pm10", "measurement", NULL, false},
    {"us_aqi", "PM AQI Estimate", "us_aqi", NULL, "aqi", "measurement", NULL, false},
    {"us_aqi_level", "PM AQI Estimate Level", "us_aqi_level", NULL, NULL, NULL, NULL, false},
    {"us_aqi_primary_pollutant", "PM AQI Dominant Pollutant", "us_aqi_primary_pollutant", NULL, NULL, NULL, NULL, false},
    {"overall_air_quality", "Composite Air Quality", "overall_air_quality", NULL, NULL, NULL, NULL, false},
    {"overall_air_quality_basis", "Composite Air Quality Basis", "overall_air_quality_basis", NULL, NULL, NULL, NULL, false},
    {"overall_air_quality_driver", "Composite Air Quality Driver", "overall_air_quality_driver", NULL, NULL, NULL, NULL, false},
    {"overall_air_quality_note", "Composite Air Quality Note", "overall_air_quality_note", NULL, NULL, NULL, NULL, false},
    {"particle_profile", "Particle Profile", "particle_profile", NULL, NULL, NULL, NULL, false},
    {"particle_profile_note", "Particle Profile Note", "particle_profile_note", NULL, NULL, NULL, NULL, false},
    {"particles_0_5um", "Particles <0.5µm", "particles_0_5um", "#/cm³", NULL, "measurement", NULL, false},
    {"particles_1_0um", "Particles <1.0µm", "particles_1_0um", "#/cm³", NULL, "measurement", NULL, false},
    {"particles_2_5um", "Particles <2.5µm", "particles_2_5um", "#/cm³", NULL, "measurement", NULL, false},
    {"particles_4_0um", "Particles <4.0µm", "particles_4_0um", "#/cm³", NULL, "measurement", NULL, false},
    {"particles_10_0um", "Particles <10.0µm", "particles_10_0um", "#/cm³", NULL, "measurement", NULL, false},
    {"typical_particle_size_um", "Typical Particle Size", "typical_particle_size_um", "µm", NULL, "measurement", NULL, false},
    {"sample_age_sec", "Sample Age", "sample_age_sec", "s", "duration", "measurement", "diagnostic", true},
    {"pressure_trend_span_min", "Pressure Trend History", "pressure_trend_span_min", "min", NULL, "measurement", NULL, false},
    {"humidity_trend_span_min", "Humidity Trend History", "humidity_trend_span_min", "min", NULL, "measurement", NULL, false},
    {"wifi_rssi", "Wi-Fi RSSI", "wifi_rssi", "dBm", "signal_strength", "measurement", "diagnostic", true},
    {"uptime_sec", "Uptime", "uptime_sec", "s", "duration", "measurement", "diagnostic", true},
    {"heap_free", "Heap Free", "heap_free", "B", "data_size", "measurement", "diagnostic", true},
    {"ip_addr", "IP Address", "ip_addr", NULL, NULL, NULL, "diagnostic", true},
    {"ap_ssid", "Provisioning AP SSID", "ap_ssid", NULL, NULL, NULL, "diagnostic", true},
    {"device_id", "Device ID", "device_id", NULL, NULL, NULL, "diagnostic", true},
    {"firmware_version", "Firmware Version", "firmware_version", NULL, NULL, NULL, "diagnostic", true},
    {"last_error", "Last Error", "last_error", NULL, NULL, NULL, "diagnostic", true},
    {"us_aqi_level_key", "PM AQI Estimate Level Key", "us_aqi_level_key", NULL, NULL, NULL, "diagnostic", false},
    {"overall_air_quality_key", "Composite Air Quality Key", "overall_air_quality_key", NULL, NULL, NULL, "diagnostic", false},
    {"particle_profile_key", "Particle Profile Key", "particle_profile_key", NULL, NULL, NULL, "diagnostic", false},
};

static const binary_sensor_entity_t BINARY_SENSOR_ENTITIES[] = {
    {"provisioning_mode", "Provisioning Mode", "provisioning_mode", "running", "diagnostic", true},
    {"wifi_connected", "Wi-Fi Connected", "wifi_connected", "connectivity", "diagnostic", true},
    {"mqtt_connected", "MQTT Connected", "mqtt_connected", "connectivity", "diagnostic", true},
    {"sensors_ready", "All Sensors Ready", "sensors_ready", NULL, "diagnostic", true},
    {"scd41_ready", "SCD41 Ready", "scd41_ready", "connectivity", "diagnostic", true},
    {"sgp41_ready", "SGP41 Ready", "sgp41_ready", "connectivity", "diagnostic", true},
    {"bmp390_ready", "BMP390 Ready", "bmp390_ready", "connectivity", "diagnostic", true},
    {"sps30_ready", "SPS30 Ready", "sps30_ready", "connectivity", "diagnostic", true},
    {"status_led_ready", "Status LED Ready", "status_led_ready", NULL, "diagnostic", true},
    {"scd41_valid", "SCD41 Sample Valid", "scd41_valid", NULL, "diagnostic", false},
    {"sgp41_valid", "SGP41 Sample Valid", "sgp41_valid", NULL, "diagnostic", false},
    {"sgp41_conditioning", "SGP41 Conditioning", "sgp41_conditioning", "running", "diagnostic", false},
    {"sgp41_voc_valid", "SGP41 VOC Index Valid", "sgp41_voc_valid", NULL, "diagnostic", false},
    {"sgp41_nox_valid", "SGP41 NOx Index Valid", "sgp41_nox_valid", NULL, "diagnostic", false},
    {"bmp390_valid", "BMP390 Sample Valid", "bmp390_valid", NULL, "diagnostic", false},
    {"pm_valid", "Particle Sample Valid", "pm_valid", NULL, "diagnostic", false},
};

static void build_topic(char *buffer, size_t buffer_len, const char *suffix)
{
    snprintf(buffer, buffer_len, "%s/%s", s_ctx.config.topic_root, suffix);
}

static char *json_to_string(cJSON *json)
{
    char *rendered = cJSON_PrintUnformatted(json);
    return rendered;
}

static void build_unique_id(char *buffer, size_t buffer_len, const char *object_id)
{
    snprintf(buffer, buffer_len, "%s_%s", s_ctx.device_id, object_id);
}

static esp_err_t mqtt_publish_json(const char *topic, cJSON *json, bool retain)
{
    char *payload = json_to_string(json);
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_NO_MEM, TAG, "json render failed");
    int msg_id = esp_mqtt_client_publish(s_ctx.client, topic, payload, 0, 1, retain);
    free(payload);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

static void add_device_object(cJSON *root)
{
    cJSON *device = cJSON_AddObjectToObject(root, "dev");
    cJSON *identifiers = cJSON_AddArrayToObject(device, "ids");
    cJSON_AddItemToArray(identifiers, cJSON_CreateString(s_ctx.device_id));
    cJSON_AddStringToObject(device, "name", s_ctx.config.device_name);
    cJSON_AddStringToObject(device, "mf", "Custom");
    cJSON_AddStringToObject(device, "mdl", "ESP32-S3 Air Monitor");
    cJSON_AddStringToObject(device, "sw", s_ctx.firmware_version);
}

static esp_err_t publish_sensor_discovery(const sensor_entity_t *entity)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config",
             s_ctx.config.discovery_prefix, s_ctx.device_id, entity->object_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", entity->name);
    char unique_id[96];
    build_unique_id(unique_id, sizeof(unique_id), entity->object_id);
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "stat_t", entity->diagnostic ? s_ctx.diag_topic : s_ctx.state_topic);
    add_device_object(root);
    char value_template[80];
    snprintf(value_template, sizeof(value_template), "{{ value_json.%s }}", entity->json_key);
    cJSON_AddStringToObject(root, "val_tpl", value_template);
    cJSON_AddStringToObject(root, "avty_t", s_ctx.availability_topic);
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    if (entity->unit != NULL) {
        cJSON_AddStringToObject(root, "unit_of_meas", entity->unit);
    }
    if (entity->device_class != NULL) {
        cJSON_AddStringToObject(root, "dev_cla", entity->device_class);
    }
    if (entity->state_class != NULL) {
        cJSON_AddStringToObject(root, "stat_cla", entity->state_class);
    }
    if (entity->entity_category != NULL) {
        cJSON_AddStringToObject(root, "ent_cat", entity->entity_category);
    }

    esp_err_t err = mqtt_publish_json(topic, root, true);
    cJSON_Delete(root);
    return err;
}

static esp_err_t publish_binary_sensor_discovery(const binary_sensor_entity_t *entity)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/%s/config",
             s_ctx.config.discovery_prefix, s_ctx.device_id, entity->object_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", entity->name);
    char unique_id[96];
    build_unique_id(unique_id, sizeof(unique_id), entity->object_id);
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "stat_t", entity->diagnostic ? s_ctx.diag_topic : s_ctx.state_topic);
    add_device_object(root);
    char value_template[96];
    snprintf(value_template, sizeof(value_template), "{{ 'ON' if value_json.%s else 'OFF' }}", entity->json_key);
    cJSON_AddStringToObject(root, "val_tpl", value_template);
    cJSON_AddStringToObject(root, "pl_on", "ON");
    cJSON_AddStringToObject(root, "pl_off", "OFF");
    cJSON_AddStringToObject(root, "avty_t", s_ctx.availability_topic);
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    if (entity->device_class != NULL) {
        cJSON_AddStringToObject(root, "dev_cla", entity->device_class);
    }
    if (entity->entity_category != NULL) {
        cJSON_AddStringToObject(root, "ent_cat", entity->entity_category);
    }

    esp_err_t err = mqtt_publish_json(topic, root, true);
    cJSON_Delete(root);
    return err;
}

static esp_err_t publish_switch_discovery(const char *object_id, const char *name, const char *json_key, const char *command_suffix)
{
    char topic[128];
    char command_topic[128];
    snprintf(topic, sizeof(topic), "%s/switch/%s/%s/config", s_ctx.config.discovery_prefix, s_ctx.device_id, object_id);
    snprintf(command_topic, sizeof(command_topic), "%s/%s", s_ctx.cmd_prefix, command_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    char unique_id[96];
    build_unique_id(unique_id, sizeof(unique_id), object_id);
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "stat_t", s_ctx.state_topic);
    cJSON_AddStringToObject(root, "cmd_t", command_topic);
    char value_template[80];
    snprintf(value_template, sizeof(value_template), "{{ 'ON' if value_json.%s else 'OFF' }}", json_key);
    cJSON_AddStringToObject(root, "val_tpl", value_template);
    cJSON_AddStringToObject(root, "pl_on", "ON");
    cJSON_AddStringToObject(root, "pl_off", "OFF");
    cJSON_AddStringToObject(root, "stat_on", "ON");
    cJSON_AddStringToObject(root, "stat_off", "OFF");
    cJSON_AddStringToObject(root, "avty_t", s_ctx.availability_topic);
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    add_device_object(root);

    esp_err_t err = mqtt_publish_json(topic, root, true);
    cJSON_Delete(root);
    return err;
}

static esp_err_t publish_button_discovery(const char *object_id, const char *name, const char *command_suffix)
{
    char topic[128];
    char command_topic[128];
    snprintf(topic, sizeof(topic), "%s/button/%s/%s/config", s_ctx.config.discovery_prefix, s_ctx.device_id, object_id);
    snprintf(command_topic, sizeof(command_topic), "%s/%s", s_ctx.cmd_prefix, command_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    char unique_id[96];
    build_unique_id(unique_id, sizeof(unique_id), object_id);
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "cmd_t", command_topic);
    cJSON_AddStringToObject(root, "pl_prs", "PRESS");
    cJSON_AddStringToObject(root, "avty_t", s_ctx.availability_topic);
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    add_device_object(root);

    esp_err_t err = mqtt_publish_json(topic, root, true);
    cJSON_Delete(root);
    return err;
}

static esp_err_t publish_number_discovery(void)
{
    char topic[128];
    char command_topic[128];
    snprintf(topic, sizeof(topic), "%s/number/%s/scd41_frc_reference_ppm/config",
             s_ctx.config.discovery_prefix, s_ctx.device_id);
    snprintf(command_topic, sizeof(command_topic), "%s/scd41_frc_reference_ppm", s_ctx.cmd_prefix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "SCD41 FRC Reference");
    char unique_id[96];
    build_unique_id(unique_id, sizeof(unique_id), "scd41_frc_reference_ppm");
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "stat_t", s_ctx.state_topic);
    cJSON_AddStringToObject(root, "cmd_t", command_topic);
    cJSON_AddStringToObject(root, "val_tpl", "{{ value_json.scd41_frc_reference_ppm }}");
    cJSON_AddStringToObject(root, "unit_of_meas", "ppm");
    cJSON_AddNumberToObject(root, "min", 400);
    cJSON_AddNumberToObject(root, "max", 2000);
    cJSON_AddNumberToObject(root, "step", 1);
    cJSON_AddStringToObject(root, "mode", "box");
    cJSON_AddStringToObject(root, "avty_t", s_ctx.availability_topic);
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    add_device_object(root);

    esp_err_t err = mqtt_publish_json(topic, root, true);
    cJSON_Delete(root);
    return err;
}

static void subscribe_commands(void)
{
    const char *suffixes[] = {
        "restart",
        "factory_reset",
        "republish_discovery",
        "scd41_asc",
        "sps30_sleep",
        "sps30_fan_cleaning",
        "status_led",
        "scd41_frc_reference_ppm",
        "apply_scd41_frc",
    };
    char topic[128];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        snprintf(topic, sizeof(topic), "%s/%s", s_ctx.cmd_prefix, suffixes[i]);
        esp_mqtt_client_subscribe(s_ctx.client, topic, 1);
    }
}

static void handle_command(const char *topic, int topic_len, const char *data, int data_len)
{
    char payload[32] = {0};
    size_t copy_len = (size_t)data_len < sizeof(payload) - 1 ? (size_t)data_len : sizeof(payload) - 1;
    memcpy(payload, data, copy_len);

    /* Extract command suffix after "cmd_prefix/" */
    size_t prefix_len = strlen(s_ctx.cmd_prefix);
    if ((int)prefix_len + 1 >= topic_len || topic[prefix_len] != '/') {
        return;
    }
    if (strncmp(topic, s_ctx.cmd_prefix, prefix_len) != 0) {
        return;
    }
    const char *suffix = topic + prefix_len + 1;
    int suffix_len = topic_len - (int)prefix_len - 1;

    /* Helper macro for concise suffix matching. */
    #define CMD_IS(name) (suffix_len == (int)strlen(name) && strncmp(suffix, name, suffix_len) == 0)

    if (CMD_IS("restart")) {
        if (s_ctx.callbacks.restart_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.restart_requested(s_ctx.user_ctx);
        }
    } else if (CMD_IS("factory_reset")) {
        if (s_ctx.callbacks.factory_reset_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.factory_reset_requested(s_ctx.user_ctx);
        }
    } else if (CMD_IS("republish_discovery")) {
        if (s_ctx.callbacks.republish_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.republish_requested(s_ctx.user_ctx);
        }
    } else if (CMD_IS("scd41_asc")) {
        if (strcasecmp(payload, "ON") == 0 || strcasecmp(payload, "OFF") == 0) {
            bool enabled = strcasecmp(payload, "ON") == 0;
            if (s_ctx.callbacks.set_scd41_asc_requested != NULL) {
                if (s_ctx.callbacks.set_scd41_asc_requested(enabled, s_ctx.user_ctx) == ESP_OK) {
                    s_ctx.scd41_asc_enabled = enabled;
                }
            } else {
                s_ctx.scd41_asc_enabled = enabled;
            }
        }
    } else if (CMD_IS("sps30_sleep")) {
        if (strcasecmp(payload, "ON") == 0 || strcasecmp(payload, "OFF") == 0) {
            bool sleep = strcasecmp(payload, "ON") == 0;
            if (s_ctx.callbacks.set_sps30_sleep_requested != NULL) {
                s_ctx.callbacks.set_sps30_sleep_requested(sleep, s_ctx.user_ctx);
            }
        }
    } else if (CMD_IS("sps30_fan_cleaning")) {
        if (s_ctx.callbacks.start_sps30_fan_cleaning_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.start_sps30_fan_cleaning_requested(s_ctx.user_ctx);
        }
    } else if (CMD_IS("status_led")) {
        if (strcasecmp(payload, "ON") == 0 || strcasecmp(payload, "OFF") == 0) {
            bool enabled = strcasecmp(payload, "ON") == 0;
            if (s_ctx.callbacks.set_status_led_requested != NULL) {
                s_ctx.callbacks.set_status_led_requested(enabled, s_ctx.user_ctx);
            }
        }
    } else if (CMD_IS("scd41_frc_reference_ppm")) {
        uint32_t ppm = (uint32_t)strtoul(payload, NULL, 10);
        if (ppm >= 400 && ppm <= 2000) {
            esp_err_t err = ESP_OK;
            if (s_ctx.callbacks.set_scd41_frc_reference_requested != NULL) {
                err = s_ctx.callbacks.set_scd41_frc_reference_requested((uint16_t)ppm, s_ctx.user_ctx);
            }
            if (err == ESP_OK) {
                s_ctx.frc_reference_ppm = (uint16_t)ppm;
            }
        }
    } else if (CMD_IS("apply_scd41_frc")) {
        if (s_ctx.callbacks.apply_scd41_frc_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.apply_scd41_frc_requested(s_ctx.frc_reference_ppm, s_ctx.user_ctx);
        }
    }

    #undef CMD_IS
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_ctx.connected = true;
        mqtt_clear_last_error();
        ESP_LOGI(TAG, "connected to %s", s_ctx.broker_uri);
        subscribe_commands();
        mqtt_ha_publish_availability(true);
        mqtt_ha_publish_discovery();
        if (s_ctx.callbacks.connected != NULL) {
            s_ctx.callbacks.connected(s_ctx.user_ctx);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_ctx.connected = false;
        if (s_ctx.last_error[0] == '\0') {
            mqtt_set_last_error("MQTT: disconnected from broker");
        }
        ESP_LOGW(TAG, "disconnected from %s", s_ctx.broker_uri);
        break;
    case MQTT_EVENT_DATA:
        handle_command(event->topic, event->topic_len, event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        s_ctx.connected = false;
        if (event != NULL && event->error_handle != NULL) {
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                int sock_errno = event->error_handle->esp_transport_sock_errno;
                const char *sock_text = sock_errno ? strerror(sock_errno) : "n/a";
                mqtt_set_last_error("MQTT: tcp error errno=%d (%s)", sock_errno, sock_text);
                ESP_LOGW(TAG,
                         "MQTT transport error: esp-tls=0x%x tls=0x%x errno=%d (%s)",
                         event->error_handle->esp_tls_last_esp_err,
                         event->error_handle->esp_tls_stack_err,
                         sock_errno,
                         sock_text);
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                int code = event->error_handle->connect_return_code;
                mqtt_set_last_error("MQTT: connect refused (%s)", mqtt_connect_return_code_text(code));
                ESP_LOGW(TAG, "MQTT connection refused: code=0x%x (%s)", code, mqtt_connect_return_code_text(code));
            } else {
                mqtt_set_last_error("MQTT: error type=%d", event->error_handle->error_type);
                ESP_LOGW(TAG, "MQTT error type=%d", event->error_handle->error_type);
            }
        } else {
            mqtt_set_last_error("MQTT: unknown client error");
            ESP_LOGW(TAG, "MQTT event error without details");
        }
        break;
    default:
        break;
    }
}

esp_err_t mqtt_ha_start(const device_config_t *config,
                        const char *device_id,
                        const char *firmware_version,
                        const mqtt_ha_callbacks_t *callbacks,
                        void *user_ctx)
{
    mqtt_ha_stop();
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config = *config;
    s_ctx.user_ctx = user_ctx;
    s_ctx.frc_reference_ppm = 400;
    s_ctx.scd41_asc_enabled = config->scd41_asc_enabled;
    if (callbacks != NULL) {
        s_ctx.callbacks = *callbacks;
    }
    strlcpy(s_ctx.device_id, device_id, sizeof(s_ctx.device_id));
    strlcpy(s_ctx.firmware_version, firmware_version, sizeof(s_ctx.firmware_version));
    build_topic(s_ctx.state_topic, sizeof(s_ctx.state_topic), "state");
    build_topic(s_ctx.diag_topic, sizeof(s_ctx.diag_topic), "diag");
    build_topic(s_ctx.availability_topic, sizeof(s_ctx.availability_topic), "availability");
    build_topic(s_ctx.cmd_prefix, sizeof(s_ctx.cmd_prefix), "cmd");

    snprintf(s_ctx.broker_uri, sizeof(s_ctx.broker_uri), "mqtt://%s:%u", config->mqtt_host, config->mqtt_port);
    ESP_LOGI(TAG, "starting MQTT client for %s", s_ctx.broker_uri);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_ctx.broker_uri,
        .credentials.username = config->mqtt_username[0] ? config->mqtt_username : NULL,
        .credentials.authentication.password = config->mqtt_password[0] ? config->mqtt_password : NULL,
        .credentials.client_id = device_id,
        .session.keepalive = 60,
        .session.last_will.topic = s_ctx.availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .network.timeout_ms = 10000,
        .network.reconnect_timeout_ms = 5000,
    };

    s_ctx.client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_ctx.client != NULL, ESP_FAIL, TAG, "mqtt init failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_ctx.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL),
                        TAG, "register event failed");
    return esp_mqtt_client_start(s_ctx.client);
}

void mqtt_ha_stop(void)
{
    if (s_ctx.client != NULL) {
        mqtt_ha_publish_availability(false);
        esp_mqtt_client_stop(s_ctx.client);
        esp_mqtt_client_destroy(s_ctx.client);
        s_ctx.client = NULL;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}

bool mqtt_ha_is_connected(void)
{
    return s_ctx.connected;
}

void mqtt_ha_get_last_error(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    portENTER_CRITICAL(&s_error_mux);
    strlcpy(buffer, s_ctx.last_error, buffer_len);
    portEXIT_CRITICAL(&s_error_mux);
}

void mqtt_ha_set_control_state(bool scd41_asc_enabled, uint16_t frc_reference_ppm)
{
    s_ctx.scd41_asc_enabled = scd41_asc_enabled;
    s_ctx.frc_reference_ppm = frc_reference_ppm;
}

void mqtt_ha_set_device_name(const char *device_name)
{
    if (device_name == NULL) {
        return;
    }
    strlcpy(s_ctx.config.device_name, device_name, sizeof(s_ctx.config.device_name));
}

uint16_t mqtt_ha_get_frc_reference_ppm(void)
{
    return s_ctx.frc_reference_ppm;
}

esp_err_t mqtt_ha_publish_availability(bool online)
{
    if (s_ctx.client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_publish(s_ctx.client, s_ctx.availability_topic, online ? "online" : "offline", 0, 1, true) >= 0
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t mqtt_ha_publish_discovery(void)
{
    if (s_ctx.client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < sizeof(SENSOR_ENTITIES) / sizeof(SENSOR_ENTITIES[0]); ++i) {
        ESP_RETURN_ON_ERROR(publish_sensor_discovery(&SENSOR_ENTITIES[i]), TAG, "sensor discovery failed");
    }
    for (size_t i = 0; i < sizeof(BINARY_SENSOR_ENTITIES) / sizeof(BINARY_SENSOR_ENTITIES[0]); ++i) {
        ESP_RETURN_ON_ERROR(publish_binary_sensor_discovery(&BINARY_SENSOR_ENTITIES[i]), TAG, "binary sensor discovery failed");
    }
    ESP_RETURN_ON_ERROR(publish_switch_discovery("scd41_asc", "SCD41 ASC", "scd41_asc_enabled", "scd41_asc"), TAG, "asc discovery failed");
    ESP_RETURN_ON_ERROR(publish_switch_discovery("sps30_sleep", "SPS30 Sleep", "sps30_sleeping", "sps30_sleep"), TAG, "sps30 discovery failed");
    ESP_RETURN_ON_ERROR(publish_switch_discovery("status_led", "Status LED", "status_led_enabled", "status_led"), TAG, "status led discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("restart", "Restart", "restart"), TAG, "restart discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("factory_reset", "Factory Reset", "factory_reset"), TAG, "factory discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("republish_discovery", "Republish Discovery", "republish_discovery"), TAG, "republish discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("sps30_fan_cleaning", "SPS30 Fan Cleaning", "sps30_fan_cleaning"),
                        TAG,
                        "sps30 fan cleaning discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("apply_scd41_frc", "Apply SCD41 FRC", "apply_scd41_frc"), TAG, "frc button discovery failed");
    return publish_number_discovery();
}

esp_err_t mqtt_ha_publish_state(const sensor_snapshot_t *snapshot, const device_diag_t *diag)
{
    if (s_ctx.client == NULL || snapshot == NULL || diag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    air_quality_assessment_t assessment = {0};
    air_quality_compute_overall_assessment(snapshot, &assessment);
    air_quality_particle_insight_t particle = {0};
    air_quality_compute_particle_insight(snapshot, &particle);
    air_quality_rain_analysis_t rain = {0};
    air_quality_analyze_rain(snapshot, &rain);
    air_quality_signal_level_t co2_rating = AIR_QUALITY_SIGNAL_UNAVAILABLE;
    air_quality_signal_level_t voc_rating = AIR_QUALITY_SIGNAL_UNAVAILABLE;
    air_quality_signal_level_t nox_rating = AIR_QUALITY_SIGNAL_UNAVAILABLE;
    const char *temperature_rating = "Unavailable";
    const char *humidity_rating = "Unavailable";

    cJSON *state = cJSON_CreateObject();
    cJSON_AddBoolToObject(state, "scd41_valid", snapshot->scd41_valid);
    cJSON_AddBoolToObject(state, "sgp41_valid", snapshot->sgp41_valid);
    cJSON_AddBoolToObject(state, "sgp41_conditioning", snapshot->sgp41_conditioning);
    cJSON_AddBoolToObject(state, "sgp41_voc_valid", snapshot->sgp41_voc_valid);
    cJSON_AddBoolToObject(state, "sgp41_nox_valid", snapshot->sgp41_nox_valid);
    cJSON_AddBoolToObject(state, "bmp390_valid", snapshot->bmp390_valid);
    cJSON_AddBoolToObject(state, "pm_valid", snapshot->pm_valid);
    cJSON_AddStringToObject(state, "co2_compensation_source",
                            co2_compensation_source_key(snapshot->co2_compensation_source));
    cJSON_AddNumberToObject(state,
                            "sgp41_voc_stabilization_remaining_s",
                            snapshot->sgp41_voc_stabilization_remaining_s);
    cJSON_AddNumberToObject(state,
                            "sgp41_nox_stabilization_remaining_s",
                            snapshot->sgp41_nox_stabilization_remaining_s);
    if (snapshot->scd41_valid) {
        co2_rating = air_quality_rate_co2(snapshot->co2_ppm);
        temperature_rating = air_quality_rate_temperature_label(snapshot->temperature_c);
        humidity_rating = air_quality_rate_humidity_label(snapshot->humidity_rh);
        cJSON_AddNumberToObject(state, "co2", snapshot->co2_ppm);
        cJSON_AddStringToObject(state, "co2_rating", air_quality_co2_ventilation_label(co2_rating));
        cJSON_AddNumberToObject(state, "temperature", snapshot->temperature_c);
        cJSON_AddStringToObject(state, "temperature_rating", temperature_rating);
        cJSON_AddNumberToObject(state, "humidity", snapshot->humidity_rh);
        cJSON_AddStringToObject(state, "humidity_rating", humidity_rating);
    } else {
        cJSON_AddNullToObject(state, "co2");
        cJSON_AddNullToObject(state, "co2_rating");
        cJSON_AddNullToObject(state, "temperature");
        cJSON_AddNullToObject(state, "temperature_rating");
        cJSON_AddNullToObject(state, "humidity");
        cJSON_AddNullToObject(state, "humidity_rating");
    }
    if (snapshot->humidity_trend_valid) {
        cJSON_AddNumberToObject(state, "humidity_trend_rh_3h", snapshot->humidity_trend_rh_3h);
        cJSON_AddNumberToObject(state, "humidity_trend_span_min", snapshot->humidity_trend_span_min);
    } else {
        cJSON_AddNullToObject(state, "humidity_trend_rh_3h");
        cJSON_AddNullToObject(state, "humidity_trend_span_min");
    }
    if (snapshot->sgp41_voc_valid) {
        voc_rating = air_quality_rate_voc_index(snapshot->voc_index);
        cJSON_AddNumberToObject(state, "voc_index", snapshot->voc_index);
        cJSON_AddStringToObject(state, "voc_rating", air_quality_voc_event_label(voc_rating));
    } else {
        cJSON_AddNullToObject(state, "voc_index");
        cJSON_AddNullToObject(state, "voc_rating");
    }
    if (snapshot->sgp41_nox_valid) {
        nox_rating = air_quality_rate_nox_index(snapshot->nox_index);
        cJSON_AddNumberToObject(state, "nox_index", snapshot->nox_index);
        cJSON_AddStringToObject(state, "nox_rating", air_quality_nox_event_label(nox_rating));
    } else {
        cJSON_AddNullToObject(state, "nox_index");
        cJSON_AddNullToObject(state, "nox_rating");
    }
    if (snapshot->bmp390_valid) {
        cJSON_AddNumberToObject(state, "bmp390_temperature", snapshot->bmp390_temperature_c);
        cJSON_AddNumberToObject(state, "pressure", snapshot->pressure_hpa);
    } else {
        cJSON_AddNullToObject(state, "bmp390_temperature");
        cJSON_AddNullToObject(state, "pressure");
    }
    if (snapshot->pressure_trend_valid) {
        cJSON_AddNumberToObject(state, "pressure_trend_hpa_3h", snapshot->pressure_trend_hpa_3h);
        cJSON_AddNumberToObject(state, "pressure_trend_span_min", snapshot->pressure_trend_span_min);
    } else {
        cJSON_AddNullToObject(state, "pressure_trend_hpa_3h");
        cJSON_AddNullToObject(state, "pressure_trend_span_min");
    }
    if (rain.dew_point_spread_valid) {
        cJSON_AddNumberToObject(state, "dew_point_spread_c", rain.dew_point_spread_c);
    } else {
        cJSON_AddNullToObject(state, "dew_point_spread_c");
    }
    cJSON_AddStringToObject(state, "pressure_trend", air_quality_pressure_trend_label(rain.pressure_trend));
    cJSON_AddStringToObject(state, "pressure_trend_key", air_quality_pressure_trend_key(rain.pressure_trend));
    cJSON_AddStringToObject(state, "rain_outlook", air_quality_rain_outlook_label(rain.outlook));
    cJSON_AddStringToObject(state, "rain_outlook_key", air_quality_rain_outlook_key(rain.outlook));
    cJSON_AddStringToObject(state, "rain_season", air_quality_rain_season_label(rain.season));
    cJSON_AddStringToObject(state, "rain_season_key", air_quality_rain_season_key(rain.season));
    cJSON_AddStringToObject(state, "rain_basis", rain.basis[0] ? rain.basis : "Unavailable");
    if (snapshot->pm_valid) {
        cJSON_AddNumberToObject(state, "pm1_0", snapshot->pm1_0);
        cJSON_AddNumberToObject(state, "pm2_5", snapshot->pm2_5);
        cJSON_AddNumberToObject(state, "pm4_0", snapshot->pm4_0);
        cJSON_AddNumberToObject(state, "pm10_0", snapshot->pm10_0);
        cJSON_AddStringToObject(state, "particle_profile", air_quality_particle_profile_label(particle.profile));
        cJSON_AddStringToObject(state, "particle_profile_note", particle.note[0] ? particle.note : "Unavailable");
        cJSON_AddNumberToObject(state, "particles_0_5um", snapshot->particles_0_5um);
        cJSON_AddNumberToObject(state, "particles_1_0um", snapshot->particles_1_0um);
        cJSON_AddNumberToObject(state, "particles_2_5um", snapshot->particles_2_5um);
        cJSON_AddNumberToObject(state, "particles_4_0um", snapshot->particles_4_0um);
        cJSON_AddNumberToObject(state, "particles_10_0um", snapshot->particles_10_0um);
        cJSON_AddNumberToObject(state, "typical_particle_size_um", snapshot->typical_particle_size_um);
    } else {
        cJSON_AddNullToObject(state, "pm1_0");
        cJSON_AddNullToObject(state, "pm2_5");
        cJSON_AddNullToObject(state, "pm4_0");
        cJSON_AddNullToObject(state, "pm10_0");
        cJSON_AddNullToObject(state, "particle_profile");
        cJSON_AddNullToObject(state, "particle_profile_note");
        cJSON_AddNullToObject(state, "particles_0_5um");
        cJSON_AddNullToObject(state, "particles_1_0um");
        cJSON_AddNullToObject(state, "particles_2_5um");
        cJSON_AddNullToObject(state, "particles_4_0um");
        cJSON_AddNullToObject(state, "particles_10_0um");
        cJSON_AddNullToObject(state, "typical_particle_size_um");
    }
    if (assessment.us_aqi.valid) {
        cJSON_AddNumberToObject(state, "us_aqi", assessment.us_aqi.aqi);
        cJSON_AddStringToObject(state, "us_aqi_level", air_quality_category_label(assessment.us_aqi.category));
        cJSON_AddStringToObject(state, "us_aqi_level_key", air_quality_category_key(assessment.us_aqi.category));
        cJSON_AddStringToObject(state, "us_aqi_primary_pollutant", air_quality_pollutant_label(assessment.us_aqi.dominant_pollutant));
    } else {
        cJSON_AddNullToObject(state, "us_aqi");
        cJSON_AddNullToObject(state, "us_aqi_level");
        cJSON_AddStringToObject(state, "us_aqi_level_key", air_quality_category_key(AIR_QUALITY_CATEGORY_UNKNOWN));
        cJSON_AddNullToObject(state, "us_aqi_primary_pollutant");
    }
    if (assessment.valid) {
        cJSON_AddStringToObject(state, "overall_air_quality", air_quality_category_label(assessment.category));
        cJSON_AddStringToObject(state, "overall_air_quality_key", air_quality_category_key(assessment.category));
        cJSON_AddStringToObject(state, "overall_air_quality_driver", air_quality_factor_label(assessment.dominant_factor));
    } else {
        cJSON_AddNullToObject(state, "overall_air_quality");
        cJSON_AddStringToObject(state, "overall_air_quality_key", air_quality_category_key(AIR_QUALITY_CATEGORY_UNKNOWN));
        cJSON_AddNullToObject(state, "overall_air_quality_driver");
    }
    cJSON_AddStringToObject(state, "overall_air_quality_basis", assessment.basis[0] ? assessment.basis : "Unavailable");
    cJSON_AddStringToObject(state, "overall_air_quality_note", assessment.note[0] ? assessment.note : "Unavailable");
    cJSON_AddStringToObject(state, "particle_profile_key", air_quality_particle_profile_key(particle.profile));
    cJSON_AddBoolToObject(state, "sps30_sleeping", snapshot->sps30_sleeping);
    cJSON_AddBoolToObject(state, "scd41_asc_enabled", s_ctx.scd41_asc_enabled);
    cJSON_AddBoolToObject(state, "status_led_enabled", diag->status_led_enabled);
    cJSON_AddNumberToObject(state, "scd41_frc_reference_ppm", s_ctx.frc_reference_ppm);

    cJSON *diag_json = cJSON_CreateObject();
    int64_t now_ms = esp_timer_get_time() / 1000;
    cJSON_AddBoolToObject(diag_json, "provisioning_mode", diag->provisioning_mode);
    cJSON_AddBoolToObject(diag_json, "wifi_connected", diag->wifi_connected);
    cJSON_AddBoolToObject(diag_json, "mqtt_connected", diag->mqtt_connected);
    cJSON_AddBoolToObject(diag_json, "sensors_ready", diag->sensors_ready);
    cJSON_AddBoolToObject(diag_json, "scd41_ready", diag->scd41_ready);
    cJSON_AddBoolToObject(diag_json, "sgp41_ready", diag->sgp41_ready);
    cJSON_AddBoolToObject(diag_json, "bmp390_ready", diag->bmp390_ready);
    cJSON_AddStringToObject(diag_json, "co2_compensation_source",
                            co2_compensation_source_key(snapshot->co2_compensation_source));
    cJSON_AddNumberToObject(diag_json,
                            "sgp41_voc_stabilization_remaining_s",
                            snapshot->sgp41_voc_stabilization_remaining_s);
    cJSON_AddNumberToObject(diag_json,
                            "sgp41_nox_stabilization_remaining_s",
                            snapshot->sgp41_nox_stabilization_remaining_s);
    cJSON_AddBoolToObject(diag_json, "sps30_ready", diag->sps30_ready);
    cJSON_AddBoolToObject(diag_json, "status_led_ready", diag->status_led_ready);
    if (snapshot->updated_at_ms > 0 && now_ms >= snapshot->updated_at_ms) {
        cJSON_AddNumberToObject(diag_json, "sample_age_sec", (now_ms - snapshot->updated_at_ms) / 1000);
    } else {
        cJSON_AddNullToObject(diag_json, "sample_age_sec");
    }
    cJSON_AddNumberToObject(diag_json, "wifi_rssi", diag->wifi_rssi);
    cJSON_AddNumberToObject(diag_json, "uptime_sec", diag->uptime_sec);
    cJSON_AddNumberToObject(diag_json, "heap_free", diag->heap_free);
    cJSON_AddStringToObject(diag_json, "ip_addr", diag->ip_addr);
    cJSON_AddStringToObject(diag_json, "ap_ssid", diag->ap_ssid);
    cJSON_AddStringToObject(diag_json, "device_id", diag->device_id);
    cJSON_AddStringToObject(diag_json, "firmware_version", diag->firmware_version);
    cJSON_AddStringToObject(diag_json, "last_error", diag->last_error[0] ? diag->last_error : "none");

    esp_err_t err = mqtt_publish_json(s_ctx.state_topic, state, false);
    if (err == ESP_OK) {
        err = mqtt_publish_json(s_ctx.diag_topic, diag_json, false);
    }
    cJSON_Delete(state);
    cJSON_Delete(diag_json);
    return err;
}
