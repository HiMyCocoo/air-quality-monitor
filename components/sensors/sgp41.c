#include "sgp41.h"

#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SGP41_I2C_ADDR 0x59
#define I2C_TIMEOUT_MS 1000

#define CMD_CONDITIONING 0x2612
#define CMD_MEASURE_RAW_SIGNALS 0x2619
#define CMD_TURN_HEATER_OFF 0x3615
#define CMD_GET_SERIAL_NUMBER 0x3682

static uint8_t sgp41_crc_word(uint16_t value)
{
    uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        crc ^= bytes[i];
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

static esp_err_t sgp41_read_words(sgp41_t *sensor, uint16_t *words, size_t word_count)
{
    uint8_t response[9] = {0};
    size_t response_len = word_count * 3;
    ESP_RETURN_ON_FALSE(response_len <= sizeof(response), ESP_ERR_INVALID_SIZE, "sgp41", "response too long");

    ESP_RETURN_ON_ERROR(i2c_master_receive(sensor->dev_handle,
                                           response,
                                           response_len,
                                           pdMS_TO_TICKS(I2C_TIMEOUT_MS)),
                        "sgp41", "read failed");

    for (size_t i = 0; i < word_count; ++i) {
        uint16_t value = ((uint16_t)response[i * 3] << 8) | response[i * 3 + 1];
        if (response[i * 3 + 2] != sgp41_crc_word(value)) {
            return ESP_ERR_INVALID_CRC;
        }
        words[i] = value;
    }
    return ESP_OK;
}

static esp_err_t sgp41_write_command(sgp41_t *sensor, uint16_t command)
{
    uint8_t bytes[2] = {(uint8_t)(command >> 8), (uint8_t)command};
    return i2c_master_transmit(sensor->dev_handle, bytes, sizeof(bytes), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t sgp41_write_command_two_words(sgp41_t *sensor, uint16_t command, uint16_t word0, uint16_t word1)
{
    uint8_t bytes[8] = {
        (uint8_t)(command >> 8),
        (uint8_t)command,
        (uint8_t)(word0 >> 8),
        (uint8_t)word0,
        sgp41_crc_word(word0),
        (uint8_t)(word1 >> 8),
        (uint8_t)word1,
        sgp41_crc_word(word1),
    };
    return i2c_master_transmit(sensor->dev_handle, bytes, sizeof(bytes), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t sgp41_init_on_bus(sgp41_t *sensor, i2c_master_bus_handle_t bus_handle)
{
    memset(sensor, 0, sizeof(*sensor));
    sensor->bus_handle = bus_handle;
    sensor->owns_bus = false;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SGP41_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(sensor->bus_handle, &dev_config, &sensor->dev_handle);
}

esp_err_t sgp41_init(sgp41_t *sensor, int i2c_port, int sda_gpio, int scl_gpio)
{
    memset(sensor, 0, sizeof(*sensor));

    i2c_master_bus_config_t bus_config = {
        .i2c_port = i2c_port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &sensor->bus_handle), "sgp41", "new bus failed");
    sensor->owns_bus = true;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SGP41_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(sensor->bus_handle, &dev_config, &sensor->dev_handle);
    if (err != ESP_OK) {
        i2c_del_master_bus(sensor->bus_handle);
        sensor->bus_handle = NULL;
        sensor->owns_bus = false;
    }
    return err;
}

void sgp41_deinit(sgp41_t *sensor)
{
    if (sensor->dev_handle != NULL) {
        i2c_master_bus_rm_device(sensor->dev_handle);
        sensor->dev_handle = NULL;
    }
    if (sensor->owns_bus && sensor->bus_handle != NULL) {
        i2c_del_master_bus(sensor->bus_handle);
        sensor->bus_handle = NULL;
    }
    sensor->owns_bus = false;
}

esp_err_t sgp41_get_serial_number(sgp41_t *sensor, uint16_t serial_number[3])
{
    ESP_RETURN_ON_FALSE(serial_number != NULL, ESP_ERR_INVALID_ARG, "sgp41", "serial buffer null");
    ESP_RETURN_ON_ERROR(sgp41_write_command(sensor, CMD_GET_SERIAL_NUMBER), "sgp41", "serial command failed");
    vTaskDelay(pdMS_TO_TICKS(2));
    return sgp41_read_words(sensor, serial_number, 3);
}

esp_err_t sgp41_execute_conditioning(sgp41_t *sensor, uint16_t humidity_ticks, uint16_t temperature_ticks,
                                     uint16_t *sraw_voc)
{
    uint16_t response = 0;
    ESP_RETURN_ON_FALSE(sraw_voc != NULL, ESP_ERR_INVALID_ARG, "sgp41", "voc buffer null");
    ESP_RETURN_ON_ERROR(sgp41_write_command_two_words(sensor, CMD_CONDITIONING, humidity_ticks, temperature_ticks),
                        "sgp41", "conditioning command failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(sgp41_read_words(sensor, &response, 1), "sgp41", "conditioning read failed");
    *sraw_voc = response;
    return ESP_OK;
}

esp_err_t sgp41_measure_raw_signals(sgp41_t *sensor, uint16_t humidity_ticks, uint16_t temperature_ticks,
                                    uint16_t *sraw_voc, uint16_t *sraw_nox)
{
    uint16_t words[2] = {0};
    ESP_RETURN_ON_FALSE(sraw_voc != NULL && sraw_nox != NULL, ESP_ERR_INVALID_ARG, "sgp41", "output buffer null");
    ESP_RETURN_ON_ERROR(sgp41_write_command_two_words(sensor, CMD_MEASURE_RAW_SIGNALS, humidity_ticks, temperature_ticks),
                        "sgp41", "measure command failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(sgp41_read_words(sensor, words, 2), "sgp41", "measure read failed");
    *sraw_voc = words[0];
    *sraw_nox = words[1];
    return ESP_OK;
}

esp_err_t sgp41_turn_heater_off(sgp41_t *sensor)
{
    esp_err_t err = sgp41_write_command(sensor, CMD_TURN_HEATER_OFF);
    vTaskDelay(pdMS_TO_TICKS(1));
    return err;
}
