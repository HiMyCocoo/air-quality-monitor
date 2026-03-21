#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEVICE_CONFIG_VERSION 1
#define DEVICE_NAME_LEN 32
#define WIFI_SSID_LEN 32
#define WIFI_PASSWORD_LEN 64
#define MQTT_HOST_LEN 63
#define MQTT_USER_LEN 32
#define MQTT_PASSWORD_LEN 64
#define DISCOVERY_PREFIX_LEN 32
#define TOPIC_ROOT_LEN 64
#define LAST_ERROR_LEN 128
#define IP_ADDR_LEN 16
#define AP_SSID_LEN 32
#define DEVICE_ID_LEN 24
#define FIRMWARE_VERSION_LEN 32

typedef struct {
    uint32_t version;
    char device_name[DEVICE_NAME_LEN + 1];
    char wifi_ssid[WIFI_SSID_LEN + 1];
    char wifi_password[WIFI_PASSWORD_LEN + 1];
    char mqtt_host[MQTT_HOST_LEN + 1];
    char mqtt_username[MQTT_USER_LEN + 1];
    char mqtt_password[MQTT_PASSWORD_LEN + 1];
    char discovery_prefix[DISCOVERY_PREFIX_LEN + 1];
    char topic_root[TOPIC_ROOT_LEN + 1];
    uint16_t mqtt_port;
    uint16_t publish_interval_sec;
    uint16_t scd41_altitude_m;
    float scd41_temp_offset_c;
    bool scd41_asc_enabled;
    bool pms_control_pins_enabled;
} device_config_t;

typedef struct {
    bool scd41_valid;
    bool pms_valid;
    bool pms_sleeping;
    uint16_t co2_ppm;
    float temperature_c;
    float humidity_rh;
    uint16_t pm1_0;
    uint16_t pm2_5;
    uint16_t pm10_0;
    uint16_t particles_0_3um;
    uint16_t particles_0_5um;
    uint16_t particles_1_0um;
    uint16_t particles_2_5um;
    uint16_t particles_5_0um;
    uint16_t particles_10_0um;
    int64_t updated_at_ms;
} sensor_snapshot_t;

typedef struct {
    bool provisioning_mode;
    bool wifi_connected;
    bool mqtt_connected;
    bool sensors_ready;
    int wifi_rssi;
    uint32_t uptime_sec;
    uint32_t heap_free;
    char ip_addr[IP_ADDR_LEN];
    char ap_ssid[AP_SSID_LEN];
    char device_id[DEVICE_ID_LEN];
    char firmware_version[FIRMWARE_VERSION_LEN];
    char last_error[LAST_ERROR_LEN];
} device_diag_t;

typedef enum {
    PLATFORM_WIFI_MODE_OFF = 0,
    PLATFORM_WIFI_MODE_AP,
    PLATFORM_WIFI_MODE_STA,
} platform_wifi_mode_t;
