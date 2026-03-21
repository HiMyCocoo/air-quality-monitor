#include "pms7003.h"

#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PMS_FRAME_LEN 32
#define PMS_BAUDRATE 9600
#define PMS_HEADER_1 0x42
#define PMS_HEADER_2 0x4D

static uint16_t read_be16(const uint8_t *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

bool pms7003_parse_frame(const uint8_t frame_bytes[32], pms7003_frame_t *frame)
{
    if (frame_bytes[0] != PMS_HEADER_1 || frame_bytes[1] != PMS_HEADER_2) {
        return false;
    }

    uint16_t frame_length = read_be16(&frame_bytes[2]);
    if (frame_length != 28) {
        return false;
    }

    uint16_t checksum = 0;
    for (size_t i = 0; i < PMS_FRAME_LEN - 2; ++i) {
        checksum += frame_bytes[i];
    }
    if (checksum != read_be16(&frame_bytes[PMS_FRAME_LEN - 2])) {
        return false;
    }

    frame->pm1_0 = read_be16(&frame_bytes[10]);
    frame->pm2_5 = read_be16(&frame_bytes[12]);
    frame->pm10_0 = read_be16(&frame_bytes[14]);
    frame->particles_0_3um = read_be16(&frame_bytes[16]);
    frame->particles_0_5um = read_be16(&frame_bytes[18]);
    frame->particles_1_0um = read_be16(&frame_bytes[20]);
    frame->particles_2_5um = read_be16(&frame_bytes[22]);
    frame->particles_5_0um = read_be16(&frame_bytes[24]);
    frame->particles_10_0um = read_be16(&frame_bytes[26]);
    return true;
}

esp_err_t pms7003_init(pms7003_t *sensor, uart_port_t uart_port, int tx_gpio, int rx_gpio, bool use_control_pins, int set_gpio, int reset_gpio)
{
    memset(sensor, 0, sizeof(*sensor));
    sensor->uart_port = uart_port;
    sensor->use_control_pins = use_control_pins;
    sensor->set_gpio = set_gpio;
    sensor->reset_gpio = reset_gpio;

    const uart_config_t uart_config = {
        .baud_rate = PMS_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(sensor->uart_port, 2048, 0, 0, NULL, 0), "pms7003", "uart install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(sensor->uart_port, &uart_config), "pms7003", "uart param failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(sensor->uart_port, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        "pms7003", "uart pins failed");
    ESP_RETURN_ON_ERROR(uart_flush(sensor->uart_port), "pms7003", "uart flush failed");

    if (use_control_pins) {
        gpio_config_t io_config = {
            .pin_bit_mask = (1ULL << set_gpio) | (1ULL << reset_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_down_en = 0,
            .pull_up_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_config), "pms7003", "gpio config failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(sensor->reset_gpio, 1), "pms7003", "reset high failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(sensor->set_gpio, 1), "pms7003", "set high failed");
    }

    return ESP_OK;
}

void pms7003_deinit(pms7003_t *sensor)
{
    if (sensor->uart_port >= UART_NUM_0) {
        uart_driver_delete(sensor->uart_port);
    }
}

esp_err_t pms7003_poll(pms7003_t *sensor, pms7003_frame_t *frame, int timeout_ms)
{
    if (sensor->rx_len < sizeof(sensor->rx_buffer)) {
        int read = uart_read_bytes(sensor->uart_port,
                                   sensor->rx_buffer + sensor->rx_len,
                                   sizeof(sensor->rx_buffer) - sensor->rx_len,
                                   pdMS_TO_TICKS(timeout_ms));
        if (read > 0) {
            sensor->rx_len += (size_t)read;
        }
    }

    if (sensor->rx_len < PMS_FRAME_LEN) {
        return ESP_ERR_TIMEOUT;
    }

    size_t offset = 0;
    while (sensor->rx_len - offset >= PMS_FRAME_LEN) {
        if (sensor->rx_buffer[offset] == PMS_HEADER_1 && sensor->rx_buffer[offset + 1] == PMS_HEADER_2) {
            if (pms7003_parse_frame(&sensor->rx_buffer[offset], frame)) {
                size_t remaining = sensor->rx_len - (offset + PMS_FRAME_LEN);
                memmove(sensor->rx_buffer, sensor->rx_buffer + offset + PMS_FRAME_LEN, remaining);
                sensor->rx_len = remaining;
                return ESP_OK;
            }
        }
        offset++;
    }

    if (offset > 0) {
        size_t remaining = sensor->rx_len - offset;
        memmove(sensor->rx_buffer, sensor->rx_buffer + offset, remaining);
        sensor->rx_len = remaining;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t pms7003_set_sleep(pms7003_t *sensor, bool sleep)
{
    if (!sensor->use_control_pins) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return gpio_set_level(sensor->set_gpio, sleep ? 0 : 1);
}

esp_err_t pms7003_reset(pms7003_t *sensor)
{
    if (!sensor->use_control_pins) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(sensor->reset_gpio, 0), "pms7003", "reset low failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(gpio_set_level(sensor->reset_gpio, 1), "pms7003", "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_OK;
}
