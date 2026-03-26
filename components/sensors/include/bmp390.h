#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bmp3.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    bool owns_bus;
    uint8_t address;
    struct bmp3_dev dev;
    struct bmp3_settings settings;
} bmp390_t;

esp_err_t bmp390_init(bmp390_t *sensor, int i2c_port, int sda_gpio, int scl_gpio, uint8_t address);
esp_err_t bmp390_init_on_bus(bmp390_t *sensor, i2c_master_bus_handle_t bus_handle, uint8_t address);
void bmp390_deinit(bmp390_t *sensor);
esp_err_t bmp390_data_ready(bmp390_t *sensor, bool *ready);
esp_err_t bmp390_read_measurement(bmp390_t *sensor, float *temperature_c, float *pressure_hpa);
