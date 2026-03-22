#include "sps30.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SPS30_SHDLC_ADDR 0x00
#define SPS30_BAUDRATE_DEFAULT 115200

#define SPS30_CMD_START_MEASUREMENT 0x00
#define SPS30_CMD_STOP_MEASUREMENT 0x01
#define SPS30_CMD_READ_MEASUREMENT 0x03
#define SPS30_CMD_SLEEP 0x10
#define SPS30_CMD_WAKE_UP 0x11
#define SPS30_CMD_START_FAN_CLEANING 0x56

#define SPS30_SUBCMD_MEASUREMENT_START_0 0x01
#define SPS30_SUBCMD_MEASUREMENT_START_1 0x03

#define SPS30_UART_RX_TIMEOUT_MS 120
#define SPS30_UART_TX_TIMEOUT_MS 100
#define SPS30_MEASUREMENT_FLOAT_COUNT 10
#define SPS30_MEASUREMENT_RESPONSE_LEN (SPS30_MEASUREMENT_FLOAT_COUNT * 4)

#define SHDLC_START 0x7E
#define SHDLC_STOP 0x7E
#define SHDLC_ESC 0x7D
#define SHDLC_MAX_DATA_LEN 255
#define SHDLC_TX_FRAME_MAX_SIZE (2 + (4 + SHDLC_MAX_DATA_LEN) * 2)
#define SHDLC_RX_FRAME_MAX_SIZE (2 + (5 + SHDLC_MAX_DATA_LEN) * 2)

typedef struct {
    uint8_t addr;
    uint8_t cmd;
    uint8_t state;
    uint8_t data_len;
} sps30_shdlc_rx_header_t;

static uint8_t sps30_shdlc_crc(uint8_t header_sum, uint8_t data_len, const uint8_t *data)
{
    header_sum += data_len;
    while (data_len--) {
        header_sum += *(data++);
    }
    return (uint8_t)~header_sum;
}

static uint16_t sps30_shdlc_stuff_data(uint8_t data_len, const uint8_t *data, uint8_t *stuffed_data)
{
    uint16_t output_data_len = 0;

    while (data_len--) {
        uint8_t value = *(data++);
        switch (value) {
        case 0x11:
        case 0x13:
        case SHDLC_ESC:
        case SHDLC_START:
            *(stuffed_data++) = SHDLC_ESC;
            *(stuffed_data++) = value ^ (1U << 5);
            output_data_len += 2;
            break;
        default:
            *(stuffed_data++) = value;
            output_data_len += 1;
            break;
        }
    }

    return output_data_len;
}

static bool sps30_shdlc_is_escape(uint8_t value)
{
    return value == SHDLC_ESC;
}

static uint8_t sps30_shdlc_unstuff_byte(uint8_t value)
{
    switch (value) {
    case 0x31:
        return 0x11;
    case 0x33:
        return 0x13;
    case 0x5D:
        return SHDLC_ESC;
    case 0x5E:
        return SHDLC_START;
    default:
        return value;
    }
}

static float sps30_bytes_to_float(const uint8_t *bytes)
{
    union {
        uint32_t u32;
        float f32;
    } value = {
        .u32 = ((uint32_t)bytes[0] << 24) |
               ((uint32_t)bytes[1] << 16) |
               ((uint32_t)bytes[2] << 8) |
               bytes[3],
    };
    return value.f32;
}

static esp_err_t sps30_uart_write_raw(sps30_t *sensor, const uint8_t *data, size_t data_len)
{
    int written = uart_write_bytes(sensor->uart_port, data, data_len);
    ESP_RETURN_ON_FALSE(written == (int)data_len, ESP_FAIL, "sps30", "uart write incomplete");
    return uart_wait_tx_done(sensor->uart_port, pdMS_TO_TICKS(SPS30_UART_TX_TIMEOUT_MS));
}

static esp_err_t sps30_uart_read_frame(sps30_t *sensor, uint8_t *buffer, size_t *frame_len)
{
    uint8_t byte = 0;
    size_t len = 0;
    int read = 0;

    while (true) {
        read = uart_read_bytes(sensor->uart_port, &byte, 1, pdMS_TO_TICKS(SPS30_UART_RX_TIMEOUT_MS));
        ESP_RETURN_ON_FALSE(read == 1, ESP_ERR_TIMEOUT, "sps30", "frame start timeout");
        if (byte == SHDLC_START) {
            buffer[len++] = byte;
            break;
        }
    }

    while (len < SHDLC_RX_FRAME_MAX_SIZE) {
        read = uart_read_bytes(sensor->uart_port, &byte, 1, pdMS_TO_TICKS(SPS30_UART_RX_TIMEOUT_MS));
        ESP_RETURN_ON_FALSE(read == 1, ESP_ERR_TIMEOUT, "sps30", "frame truncated");
        buffer[len++] = byte;
        if (byte == SHDLC_STOP) {
            *frame_len = len;
            return ESP_OK;
        }
    }

    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t sps30_shdlc_tx(sps30_t *sensor, uint8_t addr, uint8_t cmd, uint8_t data_len, const uint8_t *data)
{
    uint8_t frame[SHDLC_TX_FRAME_MAX_SIZE];
    size_t len = 0;
    uint8_t crc = sps30_shdlc_crc((uint8_t)(addr + cmd), data_len, data);

    frame[len++] = SHDLC_START;
    len += sps30_shdlc_stuff_data(1, &addr, frame + len);
    len += sps30_shdlc_stuff_data(1, &cmd, frame + len);
    len += sps30_shdlc_stuff_data(1, &data_len, frame + len);
    if (data_len > 0 && data != NULL) {
        len += sps30_shdlc_stuff_data(data_len, data, frame + len);
    }
    len += sps30_shdlc_stuff_data(1, &crc, frame + len);
    frame[len++] = SHDLC_STOP;

    uart_flush_input(sensor->uart_port);
    return sps30_uart_write_raw(sensor, frame, len);
}

static esp_err_t sps30_shdlc_rx(sps30_t *sensor, uint8_t max_data_len, sps30_shdlc_rx_header_t *header, uint8_t *data)
{
    uint8_t frame[SHDLC_RX_FRAME_MAX_SIZE] = {0};
    size_t frame_len = 0;
    uint8_t *header_bytes = (uint8_t *)header;
    uint8_t crc = 0;
    uint8_t unstuff_next = 0;
    size_t i = 0;
    uint8_t j = 0;

    ESP_RETURN_ON_ERROR(sps30_uart_read_frame(sensor, frame, &frame_len), "sps30", "frame read failed");
    ESP_RETURN_ON_FALSE(frame_len >= 7 && frame[0] == SHDLC_START, ESP_FAIL, "sps30", "missing frame start");

    for (i = 1, j = 0, unstuff_next = 0; j < sizeof(*header) && i < frame_len - 2; ++i) {
        if (unstuff_next) {
            header_bytes[j++] = sps30_shdlc_unstuff_byte(frame[i]);
            unstuff_next = 0;
        } else {
            unstuff_next = sps30_shdlc_is_escape(frame[i]);
            if (!unstuff_next) {
                header_bytes[j++] = frame[i];
            }
        }
    }
    ESP_RETURN_ON_FALSE(j == sizeof(*header) && !unstuff_next, ESP_ERR_INVALID_RESPONSE, "sps30", "header decode failed");
    ESP_RETURN_ON_FALSE(header->data_len <= max_data_len, ESP_ERR_INVALID_SIZE, "sps30", "response too long");

    for (j = 0, unstuff_next = 0; j < header->data_len && i < frame_len - 2; ++i) {
        if (unstuff_next) {
            data[j++] = sps30_shdlc_unstuff_byte(frame[i]);
            unstuff_next = 0;
        } else {
            unstuff_next = sps30_shdlc_is_escape(frame[i]);
            if (!unstuff_next) {
                data[j++] = frame[i];
            }
        }
    }
    ESP_RETURN_ON_FALSE(!unstuff_next && j == header->data_len, ESP_ERR_INVALID_RESPONSE, "sps30", "payload decode failed");

    crc = frame[i++];
    if (sps30_shdlc_is_escape(crc)) {
        crc = sps30_shdlc_unstuff_byte(frame[i++]);
    }
    ESP_RETURN_ON_FALSE(sps30_shdlc_crc((uint8_t)(header->addr + header->cmd + header->state),
                                        header->data_len,
                                        data) == crc,
                        ESP_ERR_INVALID_CRC,
                        "sps30",
                        "crc mismatch");
    ESP_RETURN_ON_FALSE(i < frame_len && frame[i] == SHDLC_STOP, ESP_ERR_INVALID_RESPONSE, "sps30", "missing frame stop");
    return ESP_OK;
}

static esp_err_t sps30_shdlc_xcv(sps30_t *sensor, uint8_t addr, uint8_t cmd, uint8_t tx_data_len, const uint8_t *tx_data,
                                 uint8_t max_rx_data_len, sps30_shdlc_rx_header_t *rx_header, uint8_t *rx_data)
{
    ESP_RETURN_ON_ERROR(sps30_shdlc_tx(sensor, addr, cmd, tx_data_len, tx_data), "sps30", "frame tx failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return sps30_shdlc_rx(sensor, max_rx_data_len, rx_header, rx_data);
}

static esp_err_t sps30_wake_up_sequence(sps30_t *sensor)
{
    uint8_t wake_byte = 0xFF;
    sps30_shdlc_rx_header_t header = {0};
    ESP_RETURN_ON_ERROR(sps30_uart_write_raw(sensor, &wake_byte, 1), "sps30", "wake byte failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(sps30_shdlc_xcv(sensor, SPS30_SHDLC_ADDR, SPS30_CMD_WAKE_UP, 0, NULL, 0, &header, NULL),
                        "sps30",
                        "wake command failed");
    sensor->sleeping = false;
    return ESP_OK;
}

esp_err_t sps30_init(sps30_t *sensor, int uart_port, int tx_gpio, int rx_gpio, uint32_t baud_rate)
{
    memset(sensor, 0, sizeof(*sensor));
    sensor->uart_port = (uart_port_t)uart_port;
    sensor->tx_gpio = tx_gpio;
    sensor->rx_gpio = rx_gpio;
    sensor->baud_rate = baud_rate == 0 ? SPS30_BAUDRATE_DEFAULT : baud_rate;

    uart_config_t uart_config = {
        .baud_rate = (int)sensor->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(sensor->uart_port, &uart_config), "sps30", "uart param config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(sensor->uart_port, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        "sps30",
                        "uart pin config failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(sensor->uart_port, 512, 0, 0, NULL, 0), "sps30", "uart driver install failed");

    sensor->initialized = true;
    sensor->last_measurement_request_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

void sps30_deinit(sps30_t *sensor)
{
    if (sensor->initialized) {
        uart_driver_delete(sensor->uart_port);
    }
    memset(sensor, 0, sizeof(*sensor));
}

esp_err_t sps30_start_measurement(sps30_t *sensor)
{
    sps30_shdlc_rx_header_t header = {0};
    uint8_t payload[2] = {SPS30_SUBCMD_MEASUREMENT_START_0, SPS30_SUBCMD_MEASUREMENT_START_1};

    ESP_RETURN_ON_ERROR(sps30_shdlc_xcv(sensor,
                                        SPS30_SHDLC_ADDR,
                                        SPS30_CMD_START_MEASUREMENT,
                                        sizeof(payload),
                                        payload,
                                        0,
                                        &header,
                                        NULL),
                        "sps30",
                        "start measurement failed");
    ESP_RETURN_ON_FALSE(header.state == 0, ESP_FAIL, "sps30", "start measurement rejected");
    sensor->measuring = true;
    sensor->sleeping = false;
    sensor->last_measurement_request_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

esp_err_t sps30_stop_measurement(sps30_t *sensor)
{
    sps30_shdlc_rx_header_t header = {0};

    ESP_RETURN_ON_ERROR(sps30_shdlc_xcv(sensor,
                                        SPS30_SHDLC_ADDR,
                                        SPS30_CMD_STOP_MEASUREMENT,
                                        0,
                                        NULL,
                                        0,
                                        &header,
                                        NULL),
                        "sps30",
                        "stop measurement failed");
    ESP_RETURN_ON_FALSE(header.state == 0, ESP_FAIL, "sps30", "stop measurement rejected");
    sensor->measuring = false;
    return ESP_OK;
}

esp_err_t sps30_data_ready(sps30_t *sensor, bool *ready)
{
    int64_t now_ms = 0;

    ESP_RETURN_ON_FALSE(ready != NULL, ESP_ERR_INVALID_ARG, "sps30", "ready null");
    if (!sensor->measuring || sensor->sleeping) {
        *ready = false;
        return ESP_OK;
    }

    /*
     * UART mode does not expose a separate data-ready query in this driver.
     * The sensor produces a new sample once per second in measurement mode, so
     * we pace read attempts at 1 Hz.
     */
    now_ms = esp_timer_get_time() / 1000;
    *ready = (now_ms - sensor->last_measurement_request_ms) >= 1000;
    return ESP_OK;
}

esp_err_t sps30_read_measurement(sps30_t *sensor, sps30_measurement_t *measurement)
{
    sps30_shdlc_rx_header_t header = {0};
    uint8_t data[SPS30_MEASUREMENT_RESPONSE_LEN] = {0};

    ESP_RETURN_ON_FALSE(measurement != NULL, ESP_ERR_INVALID_ARG, "sps30", "measurement null");
    ESP_RETURN_ON_ERROR(sps30_shdlc_xcv(sensor,
                                        SPS30_SHDLC_ADDR,
                                        SPS30_CMD_READ_MEASUREMENT,
                                        0,
                                        NULL,
                                        sizeof(data),
                                        &header,
                                        data),
                        "sps30",
                        "read measurement failed");
    ESP_RETURN_ON_FALSE(header.state == 0, ESP_FAIL, "sps30", "measurement rejected");
    ESP_RETURN_ON_FALSE(header.data_len == sizeof(data), ESP_ERR_INVALID_RESPONSE, "sps30", "bad measurement len");

    measurement->pm1_0 = sps30_bytes_to_float(&data[0]);
    measurement->pm2_5 = sps30_bytes_to_float(&data[4]);
    measurement->pm4_0 = sps30_bytes_to_float(&data[8]);
    measurement->pm10_0 = sps30_bytes_to_float(&data[12]);
    measurement->particles_0_5um = sps30_bytes_to_float(&data[16]);
    measurement->particles_1_0um = sps30_bytes_to_float(&data[20]);
    measurement->particles_2_5um = sps30_bytes_to_float(&data[24]);
    measurement->particles_4_0um = sps30_bytes_to_float(&data[28]);
    measurement->particles_10_0um = sps30_bytes_to_float(&data[32]);
    measurement->typical_particle_size_um = sps30_bytes_to_float(&data[36]);
    sensor->last_measurement_request_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

esp_err_t sps30_set_sleep(sps30_t *sensor, bool sleep)
{
    sps30_shdlc_rx_header_t header = {0};

    if (sleep) {
        if (sensor->measuring) {
            ESP_RETURN_ON_ERROR(sps30_stop_measurement(sensor), "sps30", "stop before sleep failed");
        }
        ESP_RETURN_ON_ERROR(sps30_shdlc_xcv(sensor,
                                            SPS30_SHDLC_ADDR,
                                            SPS30_CMD_SLEEP,
                                            0,
                                            NULL,
                                            0,
                                            &header,
                                            NULL),
                            "sps30",
                            "sleep command failed");
        ESP_RETURN_ON_FALSE(header.state == 0, ESP_FAIL, "sps30", "sleep rejected");
        sensor->sleeping = true;
        sensor->measuring = false;
        return ESP_OK;
    }

    if (sensor->sleeping) {
        ESP_RETURN_ON_ERROR(sps30_wake_up_sequence(sensor), "sps30", "wake sequence failed");
    }
    return sps30_start_measurement(sensor);
}

esp_err_t sps30_start_fan_cleaning(sps30_t *sensor)
{
    sps30_shdlc_rx_header_t header = {0};
    ESP_RETURN_ON_ERROR(sps30_shdlc_xcv(sensor,
                                        SPS30_SHDLC_ADDR,
                                        SPS30_CMD_START_FAN_CLEANING,
                                        0,
                                        NULL,
                                        0,
                                        &header,
                                        NULL),
                        "sps30",
                        "fan cleaning failed");
    ESP_RETURN_ON_FALSE(header.state == 0, ESP_FAIL, "sps30", "fan cleaning rejected");
    return ESP_OK;
}
