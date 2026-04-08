#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "device_types.h"
#include "esp_err.h"

typedef enum {
    DEVICE_STATE_JSON_PROFILE_MQTT = 0,
    DEVICE_STATE_JSON_PROFILE_WEB,
} device_state_json_profile_t;

typedef struct {
    bool include_sample_age_sec;
    bool include_control_state;
    bool status_led_enabled;
    bool scd41_asc_enabled;
    uint16_t scd41_frc_reference_ppm;
} device_state_json_options_t;

esp_err_t device_state_json_build_sensor_state(cJSON *json,
                                               const sensor_snapshot_t *snapshot,
                                               device_state_json_profile_t profile,
                                               const device_state_json_options_t *options);
