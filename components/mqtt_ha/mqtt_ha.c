#include "mqtt_ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
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
    esp_mqtt_client_handle_t client;
    bool connected;
    bool scd41_asc_enabled;
    uint16_t frc_reference_ppm;
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
} mqtt_ctx_t;

static mqtt_ctx_t s_ctx;

static const sensor_entity_t SENSOR_ENTITIES[] = {
    {"co2", "CO2", "co2", "ppm", "carbon_dioxide", "measurement", NULL, false},
    {"temperature", "Temperature", "temperature", "°C", "temperature", "measurement", NULL, false},
    {"humidity", "Humidity", "humidity", "%", "humidity", "measurement", NULL, false},
    {"pm1_0", "PM1.0", "pm1_0", "µg/m³", "pm1", "measurement", NULL, false},
    {"pm2_5", "PM2.5", "pm2_5", "µg/m³", "pm25", "measurement", NULL, false},
    {"pm10_0", "PM10", "pm10_0", "µg/m³", "pm10", "measurement", NULL, false},
    {"particles_0_3um", "Particles >0.3µm", "particles_0_3um", "counts/0.1L", NULL, "measurement", NULL, false},
    {"particles_0_5um", "Particles >0.5µm", "particles_0_5um", "counts/0.1L", NULL, "measurement", NULL, false},
    {"particles_1_0um", "Particles >1.0µm", "particles_1_0um", "counts/0.1L", NULL, "measurement", NULL, false},
    {"particles_2_5um", "Particles >2.5µm", "particles_2_5um", "counts/0.1L", NULL, "measurement", NULL, false},
    {"particles_5_0um", "Particles >5.0µm", "particles_5_0um", "counts/0.1L", NULL, "measurement", NULL, false},
    {"particles_10_0um", "Particles >10µm", "particles_10_0um", "counts/0.1L", NULL, "measurement", NULL, false},
    {"wifi_rssi", "Wi-Fi RSSI", "wifi_rssi", "dBm", "signal_strength", "measurement", "diagnostic", true},
    {"uptime_sec", "Uptime", "uptime_sec", "s", "duration", "measurement", "diagnostic", true},
    {"heap_free", "Heap Free", "heap_free", "B", "data_size", "measurement", "diagnostic", true},
    {"firmware_version", "Firmware Version", "firmware_version", NULL, NULL, NULL, "diagnostic", true},
    {"last_error", "Last Error", "last_error", NULL, NULL, NULL, "diagnostic", true},
};

static void build_topic(char *buffer, size_t buffer_len, const char *suffix)
{
    snprintf(buffer, buffer_len, "%s/%s", s_ctx.config.topic_root, suffix);
}

static bool topic_equals(const char *topic, int topic_len, const char *expected)
{
    return (int)strlen(expected) == topic_len && strncmp(topic, expected, topic_len) == 0;
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
    cJSON_AddStringToObject(root, "stat_on", "true");
    cJSON_AddStringToObject(root, "stat_off", "false");
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
        "pms_sleep",
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

    char expected[128];
    snprintf(expected, sizeof(expected), "%s/restart", s_ctx.cmd_prefix);
    if (topic_equals(topic, topic_len, expected)) {
        if (s_ctx.callbacks.restart_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.restart_requested(s_ctx.user_ctx);
        }
        return;
    }
    snprintf(expected, sizeof(expected), "%s/factory_reset", s_ctx.cmd_prefix);
    if (topic_equals(topic, topic_len, expected)) {
        if (s_ctx.callbacks.factory_reset_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.factory_reset_requested(s_ctx.user_ctx);
        }
        return;
    }
    snprintf(expected, sizeof(expected), "%s/republish_discovery", s_ctx.cmd_prefix);
    if (topic_equals(topic, topic_len, expected)) {
        if (s_ctx.callbacks.republish_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.republish_requested(s_ctx.user_ctx);
        }
        return;
    }
    snprintf(expected, sizeof(expected), "%s/scd41_asc", s_ctx.cmd_prefix);
    if (topic_equals(topic, topic_len, expected)) {
        bool enabled = strcasecmp(payload, "ON") == 0;
        s_ctx.scd41_asc_enabled = enabled;
        if (s_ctx.callbacks.set_scd41_asc_requested != NULL) {
            s_ctx.callbacks.set_scd41_asc_requested(enabled, s_ctx.user_ctx);
        }
        return;
    }
    snprintf(expected, sizeof(expected), "%s/pms_sleep", s_ctx.cmd_prefix);
    if (topic_equals(topic, topic_len, expected)) {
        bool sleep = strcasecmp(payload, "ON") == 0;
        if (s_ctx.callbacks.set_pms_sleep_requested != NULL) {
            s_ctx.callbacks.set_pms_sleep_requested(sleep, s_ctx.user_ctx);
        }
        return;
    }
    snprintf(expected, sizeof(expected), "%s/scd41_frc_reference_ppm", s_ctx.cmd_prefix);
    if (topic_equals(topic, topic_len, expected)) {
        uint32_t ppm = (uint32_t)strtoul(payload, NULL, 10);
        if (ppm >= 400 && ppm <= 2000) {
            s_ctx.frc_reference_ppm = (uint16_t)ppm;
        }
        return;
    }
    snprintf(expected, sizeof(expected), "%s/apply_scd41_frc", s_ctx.cmd_prefix);
    if (topic_equals(topic, topic_len, expected)) {
        if (s_ctx.callbacks.apply_scd41_frc_requested != NULL && strcmp(payload, "PRESS") == 0) {
            s_ctx.callbacks.apply_scd41_frc_requested(s_ctx.frc_reference_ppm, s_ctx.user_ctx);
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_ctx.connected = true;
        subscribe_commands();
        mqtt_ha_publish_availability(true);
        mqtt_ha_publish_discovery();
        if (s_ctx.callbacks.connected != NULL) {
            s_ctx.callbacks.connected(s_ctx.user_ctx);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_ctx.connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_command(event->topic, event->topic_len, event->data, event->data_len);
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

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_ctx.broker_uri,
        .credentials.username = config->mqtt_username[0] ? config->mqtt_username : NULL,
        .credentials.authentication.password = config->mqtt_password[0] ? config->mqtt_password : NULL,
        .credentials.client_id = device_id,
        .session.keepalive = 60,
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

void mqtt_ha_set_control_state(bool scd41_asc_enabled, uint16_t frc_reference_ppm)
{
    s_ctx.scd41_asc_enabled = scd41_asc_enabled;
    s_ctx.frc_reference_ppm = frc_reference_ppm;
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
    ESP_RETURN_ON_ERROR(publish_switch_discovery("scd41_asc", "SCD41 ASC", "scd41_asc_enabled", "scd41_asc"), TAG, "asc discovery failed");
    ESP_RETURN_ON_ERROR(publish_switch_discovery("pms_sleep", "PMS7003 Sleep", "pms_sleeping", "pms_sleep"), TAG, "pms discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("restart", "Restart", "restart"), TAG, "restart discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("factory_reset", "Factory Reset", "factory_reset"), TAG, "factory discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("republish_discovery", "Republish Discovery", "republish_discovery"), TAG, "republish discovery failed");
    ESP_RETURN_ON_ERROR(publish_button_discovery("apply_scd41_frc", "Apply SCD41 FRC", "apply_scd41_frc"), TAG, "frc button discovery failed");
    return publish_number_discovery();
}

esp_err_t mqtt_ha_publish_state(const sensor_snapshot_t *snapshot, const device_diag_t *diag)
{
    if (s_ctx.client == NULL || snapshot == NULL || diag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *state = cJSON_CreateObject();
    if (snapshot->scd41_valid) {
        cJSON_AddNumberToObject(state, "co2", snapshot->co2_ppm);
        cJSON_AddNumberToObject(state, "temperature", snapshot->temperature_c);
        cJSON_AddNumberToObject(state, "humidity", snapshot->humidity_rh);
    } else {
        cJSON_AddNullToObject(state, "co2");
        cJSON_AddNullToObject(state, "temperature");
        cJSON_AddNullToObject(state, "humidity");
    }
    if (snapshot->pms_valid) {
        cJSON_AddNumberToObject(state, "pm1_0", snapshot->pm1_0);
        cJSON_AddNumberToObject(state, "pm2_5", snapshot->pm2_5);
        cJSON_AddNumberToObject(state, "pm10_0", snapshot->pm10_0);
        cJSON_AddNumberToObject(state, "particles_0_3um", snapshot->particles_0_3um);
        cJSON_AddNumberToObject(state, "particles_0_5um", snapshot->particles_0_5um);
        cJSON_AddNumberToObject(state, "particles_1_0um", snapshot->particles_1_0um);
        cJSON_AddNumberToObject(state, "particles_2_5um", snapshot->particles_2_5um);
        cJSON_AddNumberToObject(state, "particles_5_0um", snapshot->particles_5_0um);
        cJSON_AddNumberToObject(state, "particles_10_0um", snapshot->particles_10_0um);
    } else {
        cJSON_AddNullToObject(state, "pm1_0");
        cJSON_AddNullToObject(state, "pm2_5");
        cJSON_AddNullToObject(state, "pm10_0");
        cJSON_AddNullToObject(state, "particles_0_3um");
        cJSON_AddNullToObject(state, "particles_0_5um");
        cJSON_AddNullToObject(state, "particles_1_0um");
        cJSON_AddNullToObject(state, "particles_2_5um");
        cJSON_AddNullToObject(state, "particles_5_0um");
        cJSON_AddNullToObject(state, "particles_10_0um");
    }
    cJSON_AddBoolToObject(state, "pms_sleeping", snapshot->pms_sleeping);
    cJSON_AddBoolToObject(state, "scd41_asc_enabled", s_ctx.scd41_asc_enabled);
    cJSON_AddNumberToObject(state, "scd41_frc_reference_ppm", s_ctx.frc_reference_ppm);

    cJSON *diag_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(diag_json, "wifi_rssi", diag->wifi_rssi);
    cJSON_AddNumberToObject(diag_json, "uptime_sec", diag->uptime_sec);
    cJSON_AddNumberToObject(diag_json, "heap_free", diag->heap_free);
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
