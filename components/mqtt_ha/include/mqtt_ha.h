#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_types.h"
#include "esp_err.h"

typedef struct {
    void (*restart_requested)(void *user_ctx);
    void (*factory_reset_requested)(void *user_ctx);
    void (*republish_requested)(void *user_ctx);
    esp_err_t (*set_scd41_asc_requested)(bool enabled, void *user_ctx);
    esp_err_t (*set_sps30_sleep_requested)(bool sleep, void *user_ctx);
    esp_err_t (*set_status_led_requested)(bool enabled, void *user_ctx);
    esp_err_t (*apply_scd41_frc_requested)(uint16_t ppm, void *user_ctx);
    void (*connected)(void *user_ctx);
} mqtt_ha_callbacks_t;

esp_err_t mqtt_ha_start(const device_config_t *config,
                        const char *device_id,
                        const char *firmware_version,
                        const mqtt_ha_callbacks_t *callbacks,
                        void *user_ctx);
void mqtt_ha_stop(void);
bool mqtt_ha_is_connected(void);
void mqtt_ha_set_control_state(bool scd41_asc_enabled, uint16_t frc_reference_ppm);
void mqtt_ha_set_device_name(const char *device_name);
uint16_t mqtt_ha_get_frc_reference_ppm(void);
esp_err_t mqtt_ha_publish_discovery(void);
esp_err_t mqtt_ha_publish_state(const sensor_snapshot_t *snapshot, const device_diag_t *diag);
esp_err_t mqtt_ha_publish_availability(bool online);
