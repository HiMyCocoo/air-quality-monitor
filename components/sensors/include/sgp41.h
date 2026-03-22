#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    bool owns_bus;
} sgp41_t;

esp_err_t sgp41_init(sgp41_t *sensor, int i2c_port, int sda_gpio, int scl_gpio);
esp_err_t sgp41_init_on_bus(sgp41_t *sensor, i2c_master_bus_handle_t bus_handle);
void sgp41_deinit(sgp41_t *sensor);
esp_err_t sgp41_get_serial_number(sgp41_t *sensor, uint16_t serial_number[3]);
esp_err_t sgp41_execute_conditioning(sgp41_t *sensor, uint16_t humidity_ticks, uint16_t temperature_ticks,
                                     uint16_t *sraw_voc);
esp_err_t sgp41_measure_raw_signals(sgp41_t *sensor, uint16_t humidity_ticks, uint16_t temperature_ticks,
                                    uint16_t *sraw_voc, uint16_t *sraw_nox);
esp_err_t sgp41_turn_heater_off(sgp41_t *sensor);
