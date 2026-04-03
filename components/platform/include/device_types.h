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

typedef enum {
  CO2_COMPENSATION_SOURCE_NONE = 0,
  CO2_COMPENSATION_SOURCE_ALTITUDE,
  CO2_COMPENSATION_SOURCE_BMP390,
} co2_compensation_source_t;

static inline const char *co2_compensation_source_key(co2_compensation_source_t source)
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

typedef struct {
  bool scd41_valid;
  bool sgp41_valid;
  bool sgp41_conditioning;
  bool sgp41_voc_valid;
  bool sgp41_nox_valid;
  bool bmp390_valid;
  bool pm_valid;
  bool sps30_sleeping;
  co2_compensation_source_t co2_compensation_source;
  uint32_t sgp41_voc_stabilization_remaining_s;
  uint32_t sgp41_nox_stabilization_remaining_s;
  uint16_t co2_ppm;
  float temperature_c;
  float humidity_rh;
  float bmp390_temperature_c;
  float pressure_hpa;
  bool pressure_trend_valid;
  float pressure_trend_hpa_3h;
  uint16_t pressure_trend_span_min;
  bool humidity_trend_valid;
  float humidity_trend_rh_3h;
  uint16_t humidity_trend_span_min;
  int32_t voc_index;
  int32_t nox_index;
  float pm1_0;
  float pm2_5;
  float pm4_0;
  float pm10_0;
  float particles_0_5um;
  float particles_1_0um;
  float particles_2_5um;
  float particles_4_0um;
  float particles_10_0um;
  float typical_particle_size_um;
  int64_t updated_at_ms;
} sensor_snapshot_t;

typedef struct {
  bool provisioning_mode;
  bool wifi_connected;
  bool mqtt_connected;
  bool sensors_ready;
  bool scd41_ready;
  bool sgp41_ready;
  bool bmp390_ready;
  bool sps30_ready;
  bool status_led_ready;
  bool status_led_enabled;
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
