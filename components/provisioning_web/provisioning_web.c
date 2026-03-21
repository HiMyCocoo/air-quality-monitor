#include "provisioning_web.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "provisioning_web";

#define MQTT_PORT_MIN 1
#define MQTT_PORT_MAX 65535
#define PUBLISH_INTERVAL_MIN 5
#define PUBLISH_INTERVAL_MAX 60
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

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>Air Monitor Console</title>"
    "<style>"
    ":root{--bg:#f4efe8;--ink:#1f1a17;--muted:#756c64;--accent:#d96a2b;--panel:#fffaf4;--line:#e6d8c8;}"
    "body{margin:0;font-family:'Avenir Next','Helvetica Neue',sans-serif;background:radial-gradient(circle at top left,#fff7ec,transparent 35%),var(--bg);color:var(--ink);}"
    ".wrap{max-width:1100px;margin:0 auto;padding:24px;}h1{font-size:40px;margin:0 0 8px;letter-spacing:.04em;text-transform:uppercase;}"
    ".lede{color:var(--muted);margin:0 0 24px;} .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:18px;}"
    ".card{background:var(--panel);border:1px solid var(--line);border-radius:22px;padding:18px;box-shadow:0 10px 24px rgba(90,58,35,.06);}"
    ".card h2{margin:0 0 14px;font-size:18px;text-transform:uppercase;letter-spacing:.08em;}.kv{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px dashed var(--line);}"
    ".kv:last-child{border-bottom:0;} .muted{color:var(--muted);} label{display:block;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin:12px 0 6px;}"
    "input,button{font:inherit;border-radius:14px;border:1px solid var(--line);padding:12px 14px;background:white;color:var(--ink);width:100%;box-sizing:border-box;}"
    "button{cursor:pointer;background:var(--accent);color:white;border:none;font-weight:600;margin-top:12px;}"
    ".ghost{background:transparent;border:1px solid var(--accent);color:var(--accent);} .row{display:grid;grid-template-columns:1fr 1fr;gap:12px;}"
    ".pill{display:inline-block;padding:6px 10px;border-radius:999px;background:#f7dece;color:#8a4315;font-size:12px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;}"
    ".footer{margin-top:18px;font-size:13px;color:var(--muted);} .upload{padding:12px;border:1px dashed var(--line);border-radius:16px;}"
    "</style></head><body><div class='wrap'><h1>Air Monitor Console</h1>"
    "<p class='lede'>Config, diagnostics, MQTT control and OTA for the ESP32-S3 air-quality node.</p>"
    "<div class='grid'>"
    "<section class='card'><h2>Status</h2><div id='status'></div></section>"
    "<section class='card'><h2>Telemetry</h2><div id='telemetry'></div></section>"
    "<section class='card'><h2>Wi-Fi & MQTT</h2>"
    "<label>Device Name</label><input id='device_name'/>"
    "<label>Wi-Fi SSID</label><input id='wifi_ssid'/>"
    "<label>Wi-Fi Password</label><input id='wifi_password' type='password'/>"
    "<label>MQTT Host</label><input id='mqtt_host'/>"
    "<div class='row'><div><label>MQTT Port</label><input id='mqtt_port' type='number'/></div><div><label>Publish Interval (s)</label><input id='publish_interval_sec' type='number'/></div></div>"
    "<label>MQTT Username</label><input id='mqtt_username'/>"
    "<label>MQTT Password</label><input id='mqtt_password' type='password'/>"
    "<label>Discovery Prefix</label><input id='discovery_prefix'/>"
    "<label>Topic Root</label><input id='topic_root'/>"
    "<button onclick='saveConfig()'>Save Config</button>"
    "</section>"
    "<section class='card'><h2>Sensor Controls</h2>"
    "<label>SCD41 Altitude Compensation (m)</label><input id='scd41_altitude_m' type='number'/>"
    "<label>SCD41 Temperature Offset (°C)</label><input id='scd41_temp_offset_c' type='number' step='0.1'/>"
    "<button onclick='toggleAsc(true)'>Enable ASC</button><button class='ghost' onclick='toggleAsc(false)'>Disable ASC</button>"
    "<button onclick='toggleSps30(false)'>Wake SPS30</button><button class='ghost' onclick='toggleSps30(true)'>Sleep SPS30</button>"
    "<label>SCD41 FRC Reference (ppm)</label><input id='frc_reference_ppm' type='number'/>"
    "<button onclick='applyFrc()'>Apply Forced Recalibration</button>"
    "<button class='ghost' onclick='republishDiscovery()'>Republish Discovery</button>"
    "</section>"
    "<section class='card'><h2>OTA Upload</h2><div class='upload'><input id='firmware' type='file'/><button onclick='uploadFirmware()'>Upload Firmware</button></div>"
    "<div class='footer'>The local admin page is intentionally unauthenticated on the trusted LAN selected for this project.</div></section>"
    "<section class='card'><h2>Device Actions</h2><button onclick='restartDevice()'>Restart Device</button><button class='ghost' onclick='factoryReset()'>Factory Reset</button></section>"
    "</div></div><script>"
    "const configFields=['device_name','wifi_ssid','wifi_password','mqtt_host','mqtt_port','mqtt_username','mqtt_password','discovery_prefix','topic_root','publish_interval_sec','scd41_altitude_m','scd41_temp_offset_c'];"
    "let configDirty=false;"
    "for(const k of configFields){const el=document.getElementById(k);if(el){el.addEventListener('input',()=>{configDirty=true;});}}"
    "async function fetchStatus(){const r=await fetch('/api/status');if(!r.ok)return;const d=await r.json();"
    "status.innerHTML=`<div class='pill'>${d.diag.provisioning_mode?'AP mode':'Station mode'}</div>`+"
    "`<div class='kv'><span>Device ID</span><strong>${d.diag.device_id}</strong></div>`+"
    "`<div class='kv'><span>IP</span><strong>${d.diag.ip_addr||'n/a'}</strong></div>`+"
    "`<div class='kv'><span>Wi-Fi RSSI</span><strong>${d.diag.wifi_rssi}</strong></div>`+"
    "`<div class='kv'><span>MQTT</span><strong>${d.diag.mqtt_connected}</strong></div>`+"
    "`<div class='kv'><span>Firmware</span><strong>${d.diag.firmware_version}</strong></div>`+"
    "`<div class='kv'><span>Last Error</span><strong>${d.diag.last_error}</strong></div>`;"
    "telemetry.innerHTML=`<div class='kv'><span>CO2</span><strong>${d.snapshot.co2_ppm ?? 'n/a'}</strong></div>`+"
    "`<div class='kv'><span>Temperature</span><strong>${d.snapshot.temperature_c ?? 'n/a'}</strong></div>`+"
    "`<div class='kv'><span>Humidity</span><strong>${d.snapshot.humidity_rh ?? 'n/a'}</strong></div>`+"
    "`<div class='kv'><span>PM1.0</span><strong>${d.snapshot.pm1_0 ?? 'n/a'}</strong></div>`+"
    "`<div class='kv'><span>PM2.5</span><strong>${d.snapshot.pm2_5 ?? 'n/a'}</strong></div>`+"
    "`<div class='kv'><span>PM4.0</span><strong>${d.snapshot.pm4_0 ?? 'n/a'}</strong></div>`+"
    "`<div class='kv'><span>PM10</span><strong>${d.snapshot.pm10_0 ?? 'n/a'}</strong></div>`+"
    "`<div class='kv'><span>Typical Particle Size</span><strong>${d.snapshot.typical_particle_size_um ?? 'n/a'}</strong></div>`+"
    "`<div class='kv'><span>SPS30 Sleep</span><strong>${d.snapshot.sps30_sleeping}</strong></div>`;"
    "if(!configDirty){for(const k of configFields){const el=document.getElementById(k);if(el)el.value=d.config[k] ?? '';}}"
    "if(document.activeElement!==frc_reference_ppm){frc_reference_ppm.value=d.frc_reference_ppm;}}"
    "async function saveConfig(){const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({"
    "device_name:device_name.value,wifi_ssid:wifi_ssid.value,wifi_password:wifi_password.value,mqtt_host:mqtt_host.value,mqtt_port:Number(mqtt_port.value),"
    "mqtt_username:mqtt_username.value,mqtt_password:mqtt_password.value,discovery_prefix:discovery_prefix.value,topic_root:topic_root.value,publish_interval_sec:Number(publish_interval_sec.value),"
    "scd41_altitude_m:Number(scd41_altitude_m.value),scd41_temp_offset_c:Number(scd41_temp_offset_c.value)})});"
    "if(!r.ok){alert(await r.text()||'Config rejected');return;}configDirty=false;alert('Config saved. Device will restart.');fetchStatus();}"
    "async function toggleAsc(enabled){await fetch('/api/action/scd41-asc',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled})});fetchStatus();}"
    "async function toggleSps30(sleep){await fetch('/api/action/sps30-sleep',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({sleep})});fetchStatus();}"
    "async function applyFrc(){const r=await fetch('/api/action/apply-frc',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ppm:Number(frc_reference_ppm.value)})});if(!r.ok){const t=await r.text();alert(t||'FRC rejected');}fetchStatus();}"
    "async function republishDiscovery(){await fetch('/api/action/republish-discovery',{method:'POST'});}"
    "async function restartDevice(){await fetch('/api/action/restart',{method:'POST'});}"
    "async function factoryReset(){if(confirm('Erase saved config and restart?')) await fetch('/api/action/factory-reset',{method:'POST'});}"
    "async function uploadFirmware(){const file=firmware.files[0];if(!file)return;const r=await fetch('/api/ota',{method:'POST',body:file});alert(await r.text());}"
    "fetchStatus();setInterval(fetchStatus,4000);"
    "</script></body></html>";

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

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    sensor_snapshot_t snapshot;
    device_diag_t diag;
    device_config_t config;
    uint16_t frc_ppm;
    fill_status(&snapshot, &diag, &config, &frc_ppm);

    cJSON *root = cJSON_CreateObject();
    cJSON *diag_json = cJSON_AddObjectToObject(root, "diag");
    cJSON_AddBoolToObject(diag_json, "provisioning_mode", diag.provisioning_mode);
    cJSON_AddBoolToObject(diag_json, "wifi_connected", diag.wifi_connected);
    cJSON_AddBoolToObject(diag_json, "mqtt_connected", diag.mqtt_connected);
    cJSON_AddNumberToObject(diag_json, "wifi_rssi", diag.wifi_rssi);
    cJSON_AddNumberToObject(diag_json, "uptime_sec", diag.uptime_sec);
    cJSON_AddNumberToObject(diag_json, "heap_free", diag.heap_free);
    cJSON_AddStringToObject(diag_json, "ip_addr", diag.ip_addr);
    cJSON_AddStringToObject(diag_json, "ap_ssid", diag.ap_ssid);
    cJSON_AddStringToObject(diag_json, "device_id", diag.device_id);
    cJSON_AddStringToObject(diag_json, "firmware_version", diag.firmware_version);
    cJSON_AddStringToObject(diag_json, "last_error", diag.last_error[0] ? diag.last_error : "none");

    cJSON *snapshot_json = cJSON_AddObjectToObject(root, "snapshot");
    cJSON_AddNumberToObject(snapshot_json, "co2_ppm", snapshot.co2_ppm);
    cJSON_AddNumberToObject(snapshot_json, "temperature_c", snapshot.temperature_c);
    cJSON_AddNumberToObject(snapshot_json, "humidity_rh", snapshot.humidity_rh);
    cJSON_AddNumberToObject(snapshot_json, "pm1_0", snapshot.pm1_0);
    cJSON_AddNumberToObject(snapshot_json, "pm2_5", snapshot.pm2_5);
    cJSON_AddNumberToObject(snapshot_json, "pm4_0", snapshot.pm4_0);
    cJSON_AddNumberToObject(snapshot_json, "pm10_0", snapshot.pm10_0);
    cJSON_AddNumberToObject(snapshot_json, "particles_0_5um", snapshot.particles_0_5um);
    cJSON_AddNumberToObject(snapshot_json, "particles_1_0um", snapshot.particles_1_0um);
    cJSON_AddNumberToObject(snapshot_json, "particles_2_5um", snapshot.particles_2_5um);
    cJSON_AddNumberToObject(snapshot_json, "particles_4_0um", snapshot.particles_4_0um);
    cJSON_AddNumberToObject(snapshot_json, "particles_10_0um", snapshot.particles_10_0um);
    cJSON_AddNumberToObject(snapshot_json, "typical_particle_size_um", snapshot.typical_particle_size_um);
    cJSON_AddBoolToObject(snapshot_json, "sps30_sleeping", snapshot.sps30_sleeping);

    cJSON *config_json = cJSON_AddObjectToObject(root, "config");
    cJSON_AddStringToObject(config_json, "device_name", config.device_name);
    cJSON_AddStringToObject(config_json, "wifi_ssid", config.wifi_ssid);
    cJSON_AddStringToObject(config_json, "wifi_password", config.wifi_password);
    cJSON_AddStringToObject(config_json, "mqtt_host", config.mqtt_host);
    cJSON_AddNumberToObject(config_json, "mqtt_port", config.mqtt_port);
    cJSON_AddStringToObject(config_json, "mqtt_username", config.mqtt_username);
    cJSON_AddStringToObject(config_json, "mqtt_password", config.mqtt_password);
    cJSON_AddStringToObject(config_json, "discovery_prefix", config.discovery_prefix);
    cJSON_AddStringToObject(config_json, "topic_root", config.topic_root);
    cJSON_AddNumberToObject(config_json, "publish_interval_sec", config.publish_interval_sec);
    cJSON_AddNumberToObject(config_json, "scd41_altitude_m", config.scd41_altitude_m);
    cJSON_AddNumberToObject(config_json, "scd41_temp_offset_c", config.scd41_temp_offset_c);
    cJSON_AddNumberToObject(root, "frc_reference_ppm", frc_ppm);

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (body == NULL) {
        return send_error_json(req, "400 Bad Request", "request body missing or truncated");
    }

    sensor_snapshot_t snapshot;
    device_diag_t diag;
    device_config_t config;
    uint16_t frc_ppm;
    fill_status(&snapshot, &diag, &config, &frc_ppm);

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_error_json(req, "400 Bad Request", "invalid json");
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
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_host")) && cJSON_IsString(item)) {
        strlcpy(config.mqtt_host, item->valuestring, sizeof(config.mqtt_host));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_port")) != NULL) {
        if (!whole_number_in_range(item, MQTT_PORT_MIN, MQTT_PORT_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "mqtt_port must be 1-65535");
        }
        config.mqtt_port = (uint16_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_username")) && cJSON_IsString(item)) {
        strlcpy(config.mqtt_username, item->valuestring, sizeof(config.mqtt_username));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_password")) && cJSON_IsString(item)) {
        strlcpy(config.mqtt_password, item->valuestring, sizeof(config.mqtt_password));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "discovery_prefix")) && cJSON_IsString(item)) {
        strlcpy(config.discovery_prefix, item->valuestring, sizeof(config.discovery_prefix));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "topic_root")) && cJSON_IsString(item)) {
        strlcpy(config.topic_root, item->valuestring, sizeof(config.topic_root));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "publish_interval_sec")) != NULL) {
        if (!whole_number_in_range(item, PUBLISH_INTERVAL_MIN, PUBLISH_INTERVAL_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "publish_interval_sec must be 5-60");
        }
        config.publish_interval_sec = (uint16_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "scd41_altitude_m")) != NULL) {
        if (!whole_number_in_range(item, SCD41_ALTITUDE_MIN, SCD41_ALTITUDE_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "scd41_altitude_m must be 0-3000");
        }
        config.scd41_altitude_m = (uint16_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "scd41_temp_offset_c")) != NULL) {
        if (!number_in_range(item, SCD41_TEMP_OFFSET_MIN, SCD41_TEMP_OFFSET_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "scd41_temp_offset_c must be 0-20");
        }
        config.scd41_temp_offset_c = (float)item->valuedouble;
    }
    cJSON_Delete(json);

    if (s_ctx.callbacks.save_config != NULL) {
        ESP_RETURN_ON_ERROR(s_ctx.callbacks.save_config(&config, s_ctx.user_ctx), TAG, "save config failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"restart\":true}");
}

static esp_err_t simple_action_handler(httpd_req_t *req, void (*action)(void *))
{
    if (action != NULL) {
        action(s_ctx.user_ctx);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
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
    return simple_action_handler(req, s_ctx.callbacks.request_republish_discovery);
}

static esp_err_t scd41_asc_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "body alloc failed");
    cJSON *json = cJSON_Parse(body);
    free(body);
    ESP_RETURN_ON_FALSE(json != NULL, ESP_FAIL, TAG, "invalid json");
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (s_ctx.callbacks.request_set_scd41_asc != NULL && cJSON_IsBool(enabled)) {
        s_ctx.callbacks.request_set_scd41_asc(cJSON_IsTrue(enabled), s_ctx.user_ctx);
    }
    cJSON_Delete(json);
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t sps30_sleep_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "body alloc failed");
    cJSON *json = cJSON_Parse(body);
    free(body);
    ESP_RETURN_ON_FALSE(json != NULL, ESP_FAIL, TAG, "invalid json");
    cJSON *sleep = cJSON_GetObjectItemCaseSensitive(json, "sleep");
    if (s_ctx.callbacks.request_set_sps30_sleep != NULL && cJSON_IsBool(sleep)) {
        s_ctx.callbacks.request_set_sps30_sleep(cJSON_IsTrue(sleep), s_ctx.user_ctx);
    }
    cJSON_Delete(json);
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t frc_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (body == NULL) {
        return send_error_json(req, "400 Bad Request", "request body missing or truncated");
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_error_json(req, "400 Bad Request", "invalid json");
    }
    cJSON *ppm = cJSON_GetObjectItemCaseSensitive(json, "ppm");
    if (!whole_number_in_range(ppm, 400, 2000)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "ppm must be 400-2000");
    }
    if (s_ctx.callbacks.request_apply_frc != NULL) {
        esp_err_t err = s_ctx.callbacks.request_apply_frc((uint16_t)ppm->valuedouble, s_ctx.user_ctx);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return send_error_json(req, "409 Conflict", "SCD41 FRC rejected by datasheet preconditions");
        }
    }
    cJSON_Delete(json);
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t ota_handler(httpd_req_t *req)
{
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
        ESP_RETURN_ON_ERROR(esp_ota_write(ota_handle, chunk, read), TAG, "ota write failed");
        remaining -= read;
    }

    ESP_RETURN_ON_ERROR(esp_ota_end(ota_handle), TAG, "ota end failed");
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(partition), TAG, "set boot partition failed");
    if (s_ctx.callbacks.request_restart != NULL) {
        s_ctx.callbacks.request_restart(s_ctx.user_ctx);
    }
    return httpd_resp_sendstr(req, "Firmware uploaded. Device will restart.");
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
