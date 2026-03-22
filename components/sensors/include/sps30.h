#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

typedef struct {
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
} sps30_measurement_t;

typedef struct {
    uart_port_t uart_port;
    int tx_gpio;
    int rx_gpio;
    uint32_t baud_rate;
    bool initialized;
    bool measuring;
    bool sleeping;
    int64_t last_measurement_request_ms;
} sps30_t;

esp_err_t sps30_init(sps30_t *sensor, int uart_port, int tx_gpio, int rx_gpio, uint32_t baud_rate);
void sps30_deinit(sps30_t *sensor);
esp_err_t sps30_start_measurement(sps30_t *sensor);
esp_err_t sps30_stop_measurement(sps30_t *sensor);
esp_err_t sps30_data_ready(sps30_t *sensor, bool *ready);
esp_err_t sps30_read_measurement(sps30_t *sensor, sps30_measurement_t *measurement);
esp_err_t sps30_set_sleep(sps30_t *sensor, bool sleep);
esp_err_t sps30_start_fan_cleaning(sps30_t *sensor);
