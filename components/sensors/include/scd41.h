#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
} scd41_t;

esp_err_t scd41_init(scd41_t *sensor, int i2c_port, int sda_gpio, int scl_gpio);
void scd41_deinit(scd41_t *sensor);
esp_err_t scd41_stop_periodic_measurement(scd41_t *sensor);
esp_err_t scd41_start_periodic_measurement(scd41_t *sensor);
esp_err_t scd41_set_temperature_offset(scd41_t *sensor, float offset_c);
esp_err_t scd41_set_sensor_altitude(scd41_t *sensor, uint16_t altitude_m);
esp_err_t scd41_set_automatic_self_calibration(scd41_t *sensor, bool enabled);
esp_err_t scd41_data_ready(scd41_t *sensor, bool *ready);
esp_err_t scd41_read_measurement(scd41_t *sensor, uint16_t *co2_ppm, float *temperature_c, float *humidity_rh);
esp_err_t scd41_perform_forced_recalibration(scd41_t *sensor, uint16_t target_ppm, uint16_t *correction_ppm);
