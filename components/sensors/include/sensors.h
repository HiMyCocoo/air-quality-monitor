#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_types.h"
#include "esp_err.h"

esp_err_t sensors_start(const device_config_t *config);
void sensors_stop(void);
esp_err_t sensors_get_snapshot(sensor_snapshot_t *snapshot);
bool sensors_is_ready(void);
bool sensors_is_scd41_ready(void);
bool sensors_is_sgp41_ready(void);
bool sensors_is_sps30_ready(void);
bool sensors_is_sps30_sleeping(void);
void sensors_get_last_error(char *buffer, size_t buffer_len);
esp_err_t sensors_set_scd41_compensation(uint16_t altitude_m,
                                         float temp_offset_c);
esp_err_t sensors_set_scd41_asc(bool enabled);
esp_err_t sensors_set_scd41_forced_recalibration(uint16_t reference_ppm);
esp_err_t sensors_set_sps30_sleep(bool sleep);
