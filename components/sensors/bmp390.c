#include "bmp390.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

#define BMP390_I2C_TIMEOUT_MS 1000

static const char *TAG = "bmp390";

static esp_err_t bmp390_result_to_esp_err(int8_t result)
{
    switch (result) {
    case BMP3_OK:
        return ESP_OK;
    case BMP3_E_DEV_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case BMP3_E_NULL_PTR:
        return ESP_ERR_INVALID_ARG;
    case BMP3_E_INVALID_LEN:
        return ESP_ERR_INVALID_SIZE;
    case BMP3_E_COMM_FAIL:
        return ESP_FAIL;
    default:
        return ESP_FAIL;
    }
}

static int8_t bmp390_i2c_read(uint8_t reg_addr, uint8_t *read_data, uint32_t len, void *intf_ptr)
{
    bmp390_t *sensor = (bmp390_t *)intf_ptr;
    if (sensor == NULL || sensor->dev_handle == NULL || (read_data == NULL && len > 0)) {
        return BMP3_E_NULL_PTR;
    }

    esp_err_t err = i2c_master_transmit_receive(sensor->dev_handle,
                                                &reg_addr,
                                                sizeof(reg_addr),
                                                read_data,
                                                len,
                                                pdMS_TO_TICKS(BMP390_I2C_TIMEOUT_MS));
    return err == ESP_OK ? BMP3_OK : BMP3_E_COMM_FAIL;
}

static int8_t bmp390_i2c_write(uint8_t reg_addr, const uint8_t *write_data, uint32_t len, void *intf_ptr)
{
    bmp390_t *sensor = (bmp390_t *)intf_ptr;
    if (sensor == NULL || sensor->dev_handle == NULL || (write_data == NULL && len > 0)) {
        return BMP3_E_NULL_PTR;
    }

    uint8_t *buffer = malloc(len + 1);
    if (buffer == NULL) {
        return BMP3_E_COMM_FAIL;
    }

    buffer[0] = reg_addr;
    if (len > 0) {
        memcpy(buffer + 1, write_data, len);
    }

    esp_err_t err = i2c_master_transmit(sensor->dev_handle,
                                        buffer,
                                        len + 1,
                                        pdMS_TO_TICKS(BMP390_I2C_TIMEOUT_MS));
    free(buffer);
    return err == ESP_OK ? BMP3_OK : BMP3_E_COMM_FAIL;
}

static void bmp390_delay_us(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;
    esp_rom_delay_us(period);
}

static esp_err_t bmp390_configure(bmp390_t *sensor)
{
    memset(&sensor->settings, 0, sizeof(sensor->settings));
    sensor->settings.press_en = BMP3_ENABLE;
    sensor->settings.temp_en = BMP3_ENABLE;
    sensor->settings.odr_filter.press_os = BMP3_OVERSAMPLING_8X;
    sensor->settings.odr_filter.temp_os = BMP3_OVERSAMPLING_2X;
    sensor->settings.odr_filter.iir_filter = BMP3_IIR_FILTER_COEFF_3;
    sensor->settings.odr_filter.odr = BMP3_ODR_1_5_HZ;
    sensor->settings.op_mode = BMP3_MODE_NORMAL;

    int8_t result = bmp3_set_sensor_settings(BMP3_SEL_PRESS_EN |
                                                 BMP3_SEL_TEMP_EN |
                                                 BMP3_SEL_PRESS_OS |
                                                 BMP3_SEL_TEMP_OS |
                                                 BMP3_SEL_IIR_FILTER |
                                                 BMP3_SEL_ODR,
                                             &sensor->settings,
                                             &sensor->dev);
    ESP_RETURN_ON_ERROR(bmp390_result_to_esp_err(result), "bmp390", "set sensor settings failed");

    result = bmp3_set_op_mode(&sensor->settings, &sensor->dev);
    return bmp390_result_to_esp_err(result);
}

static esp_err_t bmp390_attach_device(bmp390_t *sensor, i2c_master_bus_handle_t bus_handle, bool owns_bus, uint8_t address)
{
    memset(sensor, 0, sizeof(*sensor));
    sensor->bus_handle = bus_handle;
    sensor->owns_bus = owns_bus;
    sensor->address = address;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(sensor->bus_handle, &dev_config, &sensor->dev_handle);
    if (err != ESP_OK) {
        if (sensor->owns_bus && sensor->bus_handle != NULL) {
            i2c_del_master_bus(sensor->bus_handle);
        }
        memset(sensor, 0, sizeof(*sensor));
        return err;
    }

    sensor->dev.intf = BMP3_I2C_INTF;
    sensor->dev.intf_ptr = sensor;
    sensor->dev.read = bmp390_i2c_read;
    sensor->dev.write = bmp390_i2c_write;
    sensor->dev.delay_us = bmp390_delay_us;

    int8_t result = bmp3_init(&sensor->dev);
    if (result != BMP3_OK) {
        bmp390_deinit(sensor);
        return bmp390_result_to_esp_err(result);
    }

    err = bmp390_configure(sensor);
    if (err != ESP_OK) {
        bmp390_deinit(sensor);
    }
    return err;
}

static uint8_t bmp390_alternate_address(uint8_t address)
{
    return address == 0x76 ? 0x77 : 0x76;
}

static esp_err_t bmp390_attach_with_fallback(bmp390_t *sensor, i2c_master_bus_handle_t bus_handle, uint8_t preferred_address)
{
    esp_err_t err = bmp390_attach_device(sensor, bus_handle, false, preferred_address);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "initialized on I2C address 0x%02x", sensor->address);
        return ESP_OK;
    }

    if (err != ESP_ERR_NOT_FOUND && err != ESP_FAIL) {
        return err;
    }

    uint8_t alternate_address = bmp390_alternate_address(preferred_address);
    ESP_LOGW(TAG,
             "init failed on I2C address 0x%02x (%s), trying 0x%02x",
             preferred_address,
             esp_err_to_name(err),
             alternate_address);

    err = bmp390_attach_device(sensor, bus_handle, false, alternate_address);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "initialized on fallback I2C address 0x%02x", sensor->address);
    }
    return err;
}

esp_err_t bmp390_init_on_bus(bmp390_t *sensor, i2c_master_bus_handle_t bus_handle, uint8_t address)
{
    ESP_RETURN_ON_FALSE(sensor != NULL && bus_handle != NULL, ESP_ERR_INVALID_ARG, "bmp390", "invalid arguments");
    return bmp390_attach_with_fallback(sensor, bus_handle, address);
}

esp_err_t bmp390_init(bmp390_t *sensor, int i2c_port, int sda_gpio, int scl_gpio, uint8_t address)
{
    ESP_RETURN_ON_FALSE(sensor != NULL, ESP_ERR_INVALID_ARG, "bmp390", "sensor null");

    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = i2c_port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus_handle), "bmp390", "new bus failed");

    esp_err_t err = bmp390_attach_with_fallback(sensor, bus_handle, address);
    if (err != ESP_OK) {
        i2c_del_master_bus(bus_handle);
        return err;
    }

    sensor->owns_bus = true;
    return ESP_OK;
}

void bmp390_deinit(bmp390_t *sensor)
{
    if (sensor == NULL) {
        return;
    }
    if (sensor->dev_handle != NULL) {
        i2c_master_bus_rm_device(sensor->dev_handle);
        sensor->dev_handle = NULL;
    }
    if (sensor->owns_bus && sensor->bus_handle != NULL) {
        i2c_del_master_bus(sensor->bus_handle);
    }
    sensor->bus_handle = NULL;
    sensor->owns_bus = false;
    sensor->address = 0;
    memset(&sensor->dev, 0, sizeof(sensor->dev));
    memset(&sensor->settings, 0, sizeof(sensor->settings));
}

esp_err_t bmp390_data_ready(bmp390_t *sensor, bool *ready)
{
    ESP_RETURN_ON_FALSE(sensor != NULL && ready != NULL, ESP_ERR_INVALID_ARG, "bmp390", "invalid arguments");
    struct bmp3_status status = {0};
    int8_t result = bmp3_get_status(&status, &sensor->dev);
    ESP_RETURN_ON_ERROR(bmp390_result_to_esp_err(result), "bmp390", "get status failed");
    *ready = status.intr.drdy || (status.sensor.drdy_press && status.sensor.drdy_temp);
    return ESP_OK;
}

esp_err_t bmp390_read_measurement(bmp390_t *sensor, float *temperature_c, float *pressure_hpa)
{
    ESP_RETURN_ON_FALSE(sensor != NULL && temperature_c != NULL && pressure_hpa != NULL,
                        ESP_ERR_INVALID_ARG,
                        "bmp390",
                        "invalid arguments");
    struct bmp3_data data = {0};
    int8_t result = bmp3_get_sensor_data(BMP3_PRESS_TEMP, &data, &sensor->dev);
    ESP_RETURN_ON_ERROR(bmp390_result_to_esp_err(result), "bmp390", "read measurement failed");
    *temperature_c = (float)data.temperature;
    *pressure_hpa = (float)(data.pressure / 100.0);
    return ESP_OK;
}
