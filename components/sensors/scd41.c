#include "scd41.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCD41_I2C_ADDR 0x62
#define I2C_TIMEOUT_MS 1000

#define CMD_START_PERIODIC_MEASUREMENT 0x21B1
#define CMD_STOP_PERIODIC_MEASUREMENT 0x3F86
#define CMD_READ_MEASUREMENT 0xEC05
#define CMD_GET_DATA_READY_STATUS 0xE4B8
#define CMD_SET_TEMPERATURE_OFFSET 0x241D
#define CMD_SET_SENSOR_ALTITUDE 0x2427
#define CMD_SET_ASC 0x2416
#define CMD_PERFORM_FRC 0x362F

static uint8_t scd41_crc_word(uint16_t value) {
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

static esp_err_t scd41_write_command(scd41_t *sensor, uint16_t command) {
  uint8_t bytes[2] = {(uint8_t)(command >> 8), (uint8_t)command};
  return i2c_master_transmit(sensor->dev_handle, bytes, sizeof(bytes),
                             pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t scd41_write_command_word(scd41_t *sensor, uint16_t command,
                                          uint16_t value) {
  uint8_t bytes[5] = {
      (uint8_t)(command >> 8), (uint8_t)command,      (uint8_t)(value >> 8),
      (uint8_t)value,          scd41_crc_word(value),
  };
  return i2c_master_transmit(sensor->dev_handle, bytes, sizeof(bytes),
                             pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t scd41_read_words(scd41_t *sensor, uint16_t command,
                                  uint16_t *words, size_t word_count) {
  uint8_t command_buf[2] = {(uint8_t)(command >> 8), (uint8_t)command};
  uint8_t response[18] = {0};
  size_t response_len = word_count * 3;
  ESP_RETURN_ON_FALSE(response_len <= sizeof(response), ESP_ERR_INVALID_SIZE,
                      "scd41", "response too long");

  ESP_RETURN_ON_ERROR(
      i2c_master_transmit_receive(sensor->dev_handle, command_buf,
                                  sizeof(command_buf), response, response_len,
                                  pdMS_TO_TICKS(I2C_TIMEOUT_MS)),
      "scd41", "read failed");

  for (size_t i = 0; i < word_count; ++i) {
    uint16_t value = ((uint16_t)response[i * 3] << 8) | response[i * 3 + 1];
    if (response[i * 3 + 2] != scd41_crc_word(value)) {
      return ESP_ERR_INVALID_CRC;
    }
    words[i] = value;
  }
  return ESP_OK;
}

esp_err_t scd41_init_on_bus(scd41_t *sensor,
                            i2c_master_bus_handle_t bus_handle) {
  memset(sensor, 0, sizeof(*sensor));
  sensor->bus_handle = bus_handle;
  sensor->owns_bus = false;

  i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = SCD41_I2C_ADDR,
      .scl_speed_hz = 100000,
  };
  return i2c_master_bus_add_device(sensor->bus_handle, &dev_config,
                                   &sensor->dev_handle);
}

esp_err_t scd41_init(scd41_t *sensor, int i2c_port, int sda_gpio,
                     int scl_gpio) {
  memset(sensor, 0, sizeof(*sensor));

  i2c_master_bus_config_t bus_config = {
      .i2c_port = i2c_port,
      .sda_io_num = sda_gpio,
      .scl_io_num = scl_gpio,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &sensor->bus_handle),
                      "scd41", "new bus failed");
  sensor->owns_bus = true;

  i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = SCD41_I2C_ADDR,
      .scl_speed_hz = 100000,
  };
  esp_err_t err = i2c_master_bus_add_device(sensor->bus_handle, &dev_config,
                                            &sensor->dev_handle);
  if (err != ESP_OK) {
    i2c_del_master_bus(sensor->bus_handle);
    sensor->bus_handle = NULL;
    sensor->owns_bus = false;
  }
  return err;
}

void scd41_deinit(scd41_t *sensor) {
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

esp_err_t scd41_stop_periodic_measurement(scd41_t *sensor) {
  esp_err_t err = scd41_write_command(sensor, CMD_STOP_PERIODIC_MEASUREMENT);
  vTaskDelay(pdMS_TO_TICKS(500));
  return err;
}

esp_err_t scd41_start_periodic_measurement(scd41_t *sensor) {
  return scd41_write_command(sensor, CMD_START_PERIODIC_MEASUREMENT);
}

esp_err_t scd41_set_temperature_offset(scd41_t *sensor, float offset_c) {
  if (offset_c < 0.0f) {
    offset_c = 0.0f;
  }
  uint16_t raw = (uint16_t)lrintf((offset_c * 65535.0f) / 175.0f);
  return scd41_write_command_word(sensor, CMD_SET_TEMPERATURE_OFFSET, raw);
}

esp_err_t scd41_set_sensor_altitude(scd41_t *sensor, uint16_t altitude_m) {
  return scd41_write_command_word(sensor, CMD_SET_SENSOR_ALTITUDE, altitude_m);
}

esp_err_t scd41_set_automatic_self_calibration(scd41_t *sensor, bool enabled) {
  return scd41_write_command_word(sensor, CMD_SET_ASC, enabled ? 1 : 0);
}

esp_err_t scd41_data_ready(scd41_t *sensor, bool *ready) {
  uint16_t status = 0;
  ESP_RETURN_ON_ERROR(
      scd41_read_words(sensor, CMD_GET_DATA_READY_STATUS, &status, 1), "scd41",
      "data_ready failed");
  *ready = (status & 0x07FFU) != 0;
  return ESP_OK;
}

esp_err_t scd41_read_measurement(scd41_t *sensor, uint16_t *co2_ppm,
                                 float *temperature_c, float *humidity_rh) {
  uint16_t words[3] = {0};
  ESP_RETURN_ON_ERROR(scd41_read_words(sensor, CMD_READ_MEASUREMENT, words, 3),
                      "scd41", "read_measurement failed");
  *co2_ppm = words[0];
  *temperature_c = -45.0f + (175.0f * ((float)words[1] / 65535.0f));
  *humidity_rh = 100.0f * ((float)words[2] / 65535.0f);
  return ESP_OK;
}

esp_err_t scd41_perform_forced_recalibration(scd41_t *sensor,
                                             uint16_t target_ppm,
                                             uint16_t *correction_ppm) {
  ESP_RETURN_ON_ERROR(
      scd41_write_command_word(sensor, CMD_PERFORM_FRC, target_ppm), "scd41",
      "frc write failed");
  vTaskDelay(pdMS_TO_TICKS(400));

  /* Read 1 word (3 bytes: MSB, LSB, CRC) without re-sending the command. */
  uint8_t response[3] = {0};
  ESP_RETURN_ON_ERROR(
      i2c_master_receive(sensor->dev_handle, response, sizeof(response),
                         pdMS_TO_TICKS(I2C_TIMEOUT_MS)),
      "scd41", "frc read failed");
  uint16_t value = ((uint16_t)response[0] << 8) | response[1];
  if (response[2] != scd41_crc_word(value)) {
    return ESP_ERR_INVALID_CRC;
  }
  if (correction_ppm != NULL) {
    *correction_ppm = value;
  }
  return value == 0xFFFF ? ESP_FAIL : ESP_OK;
}
