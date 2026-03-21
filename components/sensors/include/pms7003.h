#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

typedef struct {
    uint16_t pm1_0;
    uint16_t pm2_5;
    uint16_t pm10_0;
    uint16_t particles_0_3um;
    uint16_t particles_0_5um;
    uint16_t particles_1_0um;
    uint16_t particles_2_5um;
    uint16_t particles_5_0um;
    uint16_t particles_10_0um;
} pms7003_frame_t;

typedef struct {
    uart_port_t uart_port;
    bool use_control_pins;
    gpio_num_t set_gpio;
    gpio_num_t reset_gpio;
    uint8_t rx_buffer[128];
    size_t rx_len;
} pms7003_t;

esp_err_t pms7003_init(pms7003_t *sensor, uart_port_t uart_port, int tx_gpio, int rx_gpio, bool use_control_pins, int set_gpio, int reset_gpio);
void pms7003_deinit(pms7003_t *sensor);
esp_err_t pms7003_poll(pms7003_t *sensor, pms7003_frame_t *frame, int timeout_ms);
bool pms7003_parse_frame(const uint8_t frame_bytes[32], pms7003_frame_t *frame);
esp_err_t pms7003_set_sleep(pms7003_t *sensor, bool sleep);
esp_err_t pms7003_reset(pms7003_t *sensor);
