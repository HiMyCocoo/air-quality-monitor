#include "sps30.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SPS30_I2C_ADDR 0x69
#define I2C_TIMEOUT_MS 1000

#define CMD_START_MEASUREMENT 0x0010
#define CMD_STOP_MEASUREMENT 0x0104
#define CMD_READ_DATA_READY 0x0202
#define CMD_READ_MEASUREMENT 0x0300
#define CMD_SLEEP 0x1001
#define CMD_WAKE_UP 0x1103
#define CMD_START_FAN_CLEANING 0x5607

#define SPS30_OUTPUT_FORMAT_FLOAT 0x0300
#define SPS30_MEASUREMENT_FLOAT_COUNT 10
#define SPS30_MEASUREMENT_RESPONSE_LEN (SPS30_MEASUREMENT_FLOAT_COUNT * 6)

static uint8_t sps30_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static esp_err_t sps30_write_command(sps30_t *sensor, uint16_t command)
{
    uint8_t bytes[2] = {(uint8_t)(command >> 8), (uint8_t)command};
    return i2c_master_transmit(sensor->dev_handle, bytes, sizeof(bytes), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t sps30_write_command_word(sps30_t *sensor, uint16_t command, uint16_t value)
{
    uint8_t bytes[5] = {
        (uint8_t)(command >> 8),
        (uint8_t)command,
        (uint8_t)(value >> 8),
        (uint8_t)value,
        sps30_crc((uint8_t[]){(uint8_t)(value >> 8), (uint8_t)value}, 2),
    };
    return i2c_master_transmit(sensor->dev_handle, bytes, sizeof(bytes), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t sps30_transmit_receive(sps30_t *sensor, uint16_t command, uint8_t *response, size_t response_len)
{
    uint8_t command_buf[2] = {(uint8_t)(command >> 8), (uint8_t)command};
    return i2c_master_transmit_receive(sensor->dev_handle,
                                       command_buf, sizeof(command_buf),
                                       response, response_len,
                                       pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static float sps30_parse_float(const uint8_t *response)
{
    uint32_t raw = ((uint32_t)response[0] << 24) |
                   ((uint32_t)response[1] << 16) |
                   ((uint32_t)response[3] << 8) |
                   response[4];
    union {
        uint32_t u32;
        float f32;
    } value = {
        .u32 = raw,
    };
    return value.f32;
}

static esp_err_t sps30_wake_up_sequence(sps30_t *sensor)
{
    uint8_t bytes[2] = {(uint8_t)(CMD_WAKE_UP >> 8), (uint8_t)CMD_WAKE_UP};
    (void)i2c_master_transmit(sensor->dev_handle, bytes, sizeof(bytes), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    vTaskDelay(pdMS_TO_TICKS(5));
    (void)i2c_master_transmit(sensor->dev_handle, bytes, sizeof(bytes), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    vTaskDelay(pdMS_TO_TICKS(100));
    sensor->sleeping = false;
    return ESP_OK;
}

esp_err_t sps30_init_on_bus(sps30_t *sensor, i2c_master_bus_handle_t bus_handle)
{
    memset(sensor, 0, sizeof(*sensor));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SPS30_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus_handle, &dev_config, &sensor->dev_handle);
}

void sps30_deinit(sps30_t *sensor)
{
    if (sensor->dev_handle != NULL) {
        i2c_master_bus_rm_device(sensor->dev_handle);
        sensor->dev_handle = NULL;
    }
    memset(sensor, 0, sizeof(*sensor));
}

esp_err_t sps30_start_measurement(sps30_t *sensor)
{
    ESP_RETURN_ON_ERROR(sps30_write_command_word(sensor, CMD_START_MEASUREMENT, SPS30_OUTPUT_FORMAT_FLOAT),
                        "sps30", "start measurement failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    sensor->measuring = true;
    sensor->sleeping = false;
    return ESP_OK;
}

esp_err_t sps30_stop_measurement(sps30_t *sensor)
{
    ESP_RETURN_ON_ERROR(sps30_write_command(sensor, CMD_STOP_MEASUREMENT), "sps30", "stop measurement failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    sensor->measuring = false;
    return ESP_OK;
}

esp_err_t sps30_data_ready(sps30_t *sensor, bool *ready)
{
    uint8_t response[3] = {0};
    ESP_RETURN_ON_ERROR(sps30_transmit_receive(sensor, CMD_READ_DATA_READY, response, sizeof(response)),
                        "sps30", "read data-ready failed");
    if (response[2] != sps30_crc(response, 2)) {
        return ESP_ERR_INVALID_CRC;
    }
    *ready = ((((uint16_t)response[0] << 8) | response[1]) & 0x0001U) != 0;
    return ESP_OK;
}

esp_err_t sps30_read_measurement(sps30_t *sensor, sps30_measurement_t *measurement)
{
    uint8_t response[SPS30_MEASUREMENT_RESPONSE_LEN] = {0};
    float *values = NULL;

    ESP_RETURN_ON_FALSE(measurement != NULL, ESP_ERR_INVALID_ARG, "sps30", "measurement null");
    values = &measurement->pm1_0;
    ESP_RETURN_ON_ERROR(sps30_transmit_receive(sensor, CMD_READ_MEASUREMENT, response, sizeof(response)),
                        "sps30", "read measurement failed");

    for (size_t i = 0; i < SPS30_MEASUREMENT_FLOAT_COUNT; ++i) {
        const uint8_t *chunk = &response[i * 6];
        if (chunk[2] != sps30_crc(chunk, 2) || chunk[5] != sps30_crc(chunk + 3, 2)) {
            return ESP_ERR_INVALID_CRC;
        }
        values[i] = sps30_parse_float(chunk);
    }

    return ESP_OK;
}

esp_err_t sps30_set_sleep(sps30_t *sensor, bool sleep)
{
    if (sleep) {
        if (sensor->measuring) {
            ESP_RETURN_ON_ERROR(sps30_stop_measurement(sensor), "sps30", "stop before sleep failed");
        }
        ESP_RETURN_ON_ERROR(sps30_write_command(sensor, CMD_SLEEP), "sps30", "sleep command failed");
        vTaskDelay(pdMS_TO_TICKS(5));
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
    ESP_RETURN_ON_ERROR(sps30_write_command(sensor, CMD_START_FAN_CLEANING), "sps30", "fan cleaning failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    return ESP_OK;
}
