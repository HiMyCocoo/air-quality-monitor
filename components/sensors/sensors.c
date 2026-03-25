#include "sensors.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "scd41.h"
#include "sensirion_gas_index_algorithm.h"
#include "sgp41.h"
#include "sps30.h"

#define PM_HISTORY_LEN 6
#define SCD41_FRC_STABILIZATION_MS (180000LL)
#define SGP41_CONDITIONING_SEC 10
#define SGP41_DEFAULT_HUMIDITY_TICKS 0x8000
#define SGP41_DEFAULT_TEMPERATURE_TICKS 0x6666
#define SPS30_FAN_CLEANING_DURATION_MS (10000LL)

static const char *TAG = "sensors";

typedef struct {
    bool running;
    bool scd41_initialized;
    bool sgp41_initialized;
    bool sps30_initialized;
    bool sps30_sleeping;
    TaskHandle_t task_handle;
    SemaphoreHandle_t lock;
    sensor_snapshot_t snapshot;
    char last_error[LAST_ERROR_LEN];
    char scd41_error[LAST_ERROR_LEN];
    char sgp41_error[LAST_ERROR_LEN];
    char sps30_error[LAST_ERROR_LEN];
    device_config_t config;
    scd41_t scd41;
    sgp41_t sgp41;
    sps30_t sps30;
    GasIndexAlgorithmParams sgp41_voc_params;
    GasIndexAlgorithmParams sgp41_nox_params;
    sps30_measurement_t pm_history[PM_HISTORY_LEN];
    size_t pm_history_count;
    size_t pm_history_index;
    int64_t scd41_measurement_started_ms;
    int64_t sps30_warmup_until_ms;
    uint8_t sgp41_conditioning_remaining_s;
} sensors_ctx_t;

static sensors_ctx_t s_ctx;

static void sensors_refresh_last_error_locked(void)
{
    s_ctx.last_error[0] = '\0';
    if (s_ctx.scd41_error[0] != '\0') {
        strlcpy(s_ctx.last_error, "SCD41: ", sizeof(s_ctx.last_error));
        strlcat(s_ctx.last_error, s_ctx.scd41_error, sizeof(s_ctx.last_error));
    }
    if (s_ctx.sgp41_error[0] != '\0') {
        if (s_ctx.last_error[0] != '\0') {
            strlcat(s_ctx.last_error, " | ", sizeof(s_ctx.last_error));
        }
        strlcat(s_ctx.last_error, "SGP41: ", sizeof(s_ctx.last_error));
        strlcat(s_ctx.last_error, s_ctx.sgp41_error, sizeof(s_ctx.last_error));
    }
    if (s_ctx.sps30_error[0] != '\0') {
        if (s_ctx.last_error[0] != '\0') {
            strlcat(s_ctx.last_error, " | ", sizeof(s_ctx.last_error));
        }
        strlcat(s_ctx.last_error, "SPS30: ", sizeof(s_ctx.last_error));
        strlcat(s_ctx.last_error, s_ctx.sps30_error, sizeof(s_ctx.last_error));
    }
}

static void sensors_mark_scd41_invalid_locked(void)
{
    s_ctx.snapshot.scd41_valid = false;
    s_ctx.snapshot.co2_ppm = 0;
    s_ctx.snapshot.temperature_c = 0.0f;
    s_ctx.snapshot.humidity_rh = 0.0f;
}

static void sensors_mark_sps30_invalid_locked(void)
{
    s_ctx.snapshot.pm_valid = false;
    s_ctx.snapshot.pm1_0 = 0.0f;
    s_ctx.snapshot.pm2_5 = 0.0f;
    s_ctx.snapshot.pm4_0 = 0.0f;
    s_ctx.snapshot.pm10_0 = 0.0f;
    s_ctx.snapshot.particles_0_5um = 0.0f;
    s_ctx.snapshot.particles_1_0um = 0.0f;
    s_ctx.snapshot.particles_2_5um = 0.0f;
    s_ctx.snapshot.particles_4_0um = 0.0f;
    s_ctx.snapshot.particles_10_0um = 0.0f;
    s_ctx.snapshot.typical_particle_size_um = 0.0f;
}

static void sensors_mark_sgp41_invalid_locked(void)
{
    s_ctx.snapshot.sgp41_valid = false;
    s_ctx.snapshot.sgp41_conditioning = false;
    s_ctx.snapshot.voc_index = 0;
    s_ctx.snapshot.nox_index = 0;
}

static void sensors_set_scd41_error(const char *message)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    strlcpy(s_ctx.scd41_error, message, sizeof(s_ctx.scd41_error));
    sensors_refresh_last_error_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_clear_scd41_error(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.scd41_error[0] = '\0';
    sensors_refresh_last_error_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_set_sps30_error(const char *message)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    strlcpy(s_ctx.sps30_error, message, sizeof(s_ctx.sps30_error));
    sensors_refresh_last_error_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_set_sgp41_error(const char *message)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    strlcpy(s_ctx.sgp41_error, message, sizeof(s_ctx.sgp41_error));
    sensors_refresh_last_error_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_clear_sps30_error(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.sps30_error[0] = '\0';
    sensors_refresh_last_error_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_clear_sgp41_error(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.sgp41_error[0] = '\0';
    sensors_refresh_last_error_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_mark_scd41_invalid(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    sensors_mark_scd41_invalid_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_mark_sps30_invalid(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    sensors_mark_sps30_invalid_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_mark_sgp41_invalid(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    sensors_mark_sgp41_invalid_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_reset_scd41(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.scd41.dev_handle != NULL || s_ctx.scd41.bus_handle != NULL) {
        scd41_deinit(&s_ctx.scd41);
    }
    memset(&s_ctx.scd41, 0, sizeof(s_ctx.scd41));
    s_ctx.scd41_initialized = false;
    s_ctx.scd41_measurement_started_ms = 0;
    sensors_mark_scd41_invalid_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_reset_sgp41(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.sgp41.dev_handle != NULL || s_ctx.sgp41.bus_handle != NULL) {
        sgp41_deinit(&s_ctx.sgp41);
    }
    memset(&s_ctx.sgp41, 0, sizeof(s_ctx.sgp41));
    s_ctx.sgp41_initialized = false;
    s_ctx.sgp41_conditioning_remaining_s = 0;
    sensors_mark_sgp41_invalid_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_reset_sps30(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.sps30.initialized) {
        sps30_deinit(&s_ctx.sps30);
    }
    memset(&s_ctx.sps30, 0, sizeof(s_ctx.sps30));
    s_ctx.sps30_initialized = false;
    s_ctx.sps30_sleeping = false;
    s_ctx.sps30_warmup_until_ms = 0;
    s_ctx.pm_history_count = 0;
    s_ctx.pm_history_index = 0;
    sensors_mark_sps30_invalid_locked();
    s_ctx.snapshot.sps30_sleeping = false;
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_get_sgp41_compensation_ticks(uint16_t *humidity_ticks, uint16_t *temperature_ticks)
{
    float humidity = 50.0f;
    float temperature = 25.0f;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.snapshot.scd41_valid) {
        humidity = s_ctx.snapshot.humidity_rh;
        temperature = s_ctx.snapshot.temperature_c;
    }
    xSemaphoreGive(s_ctx.lock);

    if (humidity < 0.0f) {
        humidity = 0.0f;
    } else if (humidity > 100.0f) {
        humidity = 100.0f;
    }
    if (temperature < -45.0f) {
        temperature = -45.0f;
    } else if (temperature > 130.0f) {
        temperature = 130.0f;
    }

    *humidity_ticks = (uint16_t)((humidity * 65535.0f) / 100.0f + 0.5f);
    *temperature_ticks = (uint16_t)(((temperature + 45.0f) * 65535.0f) / 175.0f + 0.5f);
}

static esp_err_t sensors_init_scd41(void)
{
    esp_err_t err = ESP_OK;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.scd41_initialized) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    memset(&s_ctx.scd41, 0, sizeof(s_ctx.scd41));
    sensors_mark_scd41_invalid_locked();

    err = scd41_init(&s_ctx.scd41,
                     CONFIG_AIRMON_SCD41_I2C_PORT_NUM,
                     CONFIG_AIRMON_SCD41_I2C_SDA_GPIO,
                     CONFIG_AIRMON_SCD41_I2C_SCL_GPIO);
    if (err == ESP_OK) {
        err = scd41_stop_periodic_measurement(&s_ctx.scd41);
    }
    if (err == ESP_OK) {
        err = scd41_set_temperature_offset(&s_ctx.scd41, s_ctx.config.scd41_temp_offset_c);
    }
    if (err == ESP_OK) {
        err = scd41_set_sensor_altitude(&s_ctx.scd41, s_ctx.config.scd41_altitude_m);
    }
    if (err == ESP_OK) {
        err = scd41_set_automatic_self_calibration(&s_ctx.scd41, s_ctx.config.scd41_asc_enabled);
    }
    if (err == ESP_OK) {
        err = scd41_start_periodic_measurement(&s_ctx.scd41);
    }
    if (err == ESP_OK) {
        s_ctx.scd41_measurement_started_ms = esp_timer_get_time() / 1000;
        s_ctx.scd41_initialized = true;
    } else if (s_ctx.scd41.dev_handle != NULL || s_ctx.scd41.bus_handle != NULL) {
        scd41_deinit(&s_ctx.scd41);
        memset(&s_ctx.scd41, 0, sizeof(s_ctx.scd41));
    }
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        sensors_clear_scd41_error();
    }
    return err;
}

static esp_err_t sensors_init_sgp41(void)
{
    uint16_t serial_number[3] = {0};
    esp_err_t err = ESP_OK;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.sgp41_initialized) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    memset(&s_ctx.sgp41, 0, sizeof(s_ctx.sgp41));
    sensors_mark_sgp41_invalid_locked();

    err = sgp41_init(&s_ctx.sgp41,
                     CONFIG_AIRMON_SGP41_I2C_PORT_NUM,
                     CONFIG_AIRMON_SGP41_I2C_SDA_GPIO,
                     CONFIG_AIRMON_SGP41_I2C_SCL_GPIO);
    if (err == ESP_OK) {
        err = sgp41_get_serial_number(&s_ctx.sgp41, serial_number);
    }
    if (err == ESP_OK) {
        GasIndexAlgorithm_init(&s_ctx.sgp41_voc_params, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
        GasIndexAlgorithm_init(&s_ctx.sgp41_nox_params, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);
        s_ctx.sgp41_conditioning_remaining_s = SGP41_CONDITIONING_SEC;
        s_ctx.sgp41_initialized = true;
        s_ctx.snapshot.sgp41_conditioning = true;
    } else if (s_ctx.sgp41.dev_handle != NULL || s_ctx.sgp41.bus_handle != NULL) {
        sgp41_deinit(&s_ctx.sgp41);
        memset(&s_ctx.sgp41, 0, sizeof(s_ctx.sgp41));
    }
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        sensors_clear_sgp41_error();
    }
    return err;
}

static esp_err_t sensors_init_sps30(void)
{
    esp_err_t err = ESP_OK;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.sps30_initialized) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    memset(&s_ctx.sps30, 0, sizeof(s_ctx.sps30));
    sensors_mark_sps30_invalid_locked();
    s_ctx.snapshot.sps30_sleeping = false;

    err = sps30_init(&s_ctx.sps30,
                     CONFIG_AIRMON_SPS30_UART_PORT_NUM,
                     CONFIG_AIRMON_SPS30_UART_TX_GPIO,
                     CONFIG_AIRMON_SPS30_UART_RX_GPIO,
                     CONFIG_AIRMON_SPS30_UART_BAUD_RATE);
    if (err == ESP_OK) {
        err = sps30_start_measurement(&s_ctx.sps30);
    }
    if (err == ESP_OK) {
        s_ctx.sps30_initialized = true;
        s_ctx.sps30_sleeping = false;
        s_ctx.sps30_warmup_until_ms =
            (esp_timer_get_time() / 1000) + (CONFIG_AIRMON_SPS30_WARMUP_SEC * 1000LL);
    } else if (s_ctx.sps30.initialized) {
        sps30_deinit(&s_ctx.sps30);
        memset(&s_ctx.sps30, 0, sizeof(s_ctx.sps30));
    }
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        sensors_clear_sps30_error();
    }
    return err;
}

static void sensors_apply_pm_average(const sps30_measurement_t *measurement)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.pm_history[s_ctx.pm_history_index] = *measurement;
    s_ctx.pm_history_index = (s_ctx.pm_history_index + 1) % PM_HISTORY_LEN;
    if (s_ctx.pm_history_count < PM_HISTORY_LEN) {
        s_ctx.pm_history_count++;
    }

    double pm1 = 0.0;
    double pm25 = 0.0;
    double pm4 = 0.0;
    double pm10 = 0.0;
    double c05 = 0.0;
    double c10 = 0.0;
    double c25 = 0.0;
    double c40 = 0.0;
    double c100 = 0.0;
    double tps = 0.0;
    for (size_t i = 0; i < s_ctx.pm_history_count; ++i) {
        pm1 += s_ctx.pm_history[i].pm1_0;
        pm25 += s_ctx.pm_history[i].pm2_5;
        pm4 += s_ctx.pm_history[i].pm4_0;
        pm10 += s_ctx.pm_history[i].pm10_0;
        c05 += s_ctx.pm_history[i].particles_0_5um;
        c10 += s_ctx.pm_history[i].particles_1_0um;
        c25 += s_ctx.pm_history[i].particles_2_5um;
        c40 += s_ctx.pm_history[i].particles_4_0um;
        c100 += s_ctx.pm_history[i].particles_10_0um;
        tps += s_ctx.pm_history[i].typical_particle_size_um;
    }

    s_ctx.snapshot.pm_valid = true;
    s_ctx.snapshot.pm1_0 = (float)(pm1 / s_ctx.pm_history_count);
    s_ctx.snapshot.pm2_5 = (float)(pm25 / s_ctx.pm_history_count);
    s_ctx.snapshot.pm4_0 = (float)(pm4 / s_ctx.pm_history_count);
    s_ctx.snapshot.pm10_0 = (float)(pm10 / s_ctx.pm_history_count);
    s_ctx.snapshot.particles_0_5um = (float)(c05 / s_ctx.pm_history_count);
    s_ctx.snapshot.particles_1_0um = (float)(c10 / s_ctx.pm_history_count);
    s_ctx.snapshot.particles_2_5um = (float)(c25 / s_ctx.pm_history_count);
    s_ctx.snapshot.particles_4_0um = (float)(c40 / s_ctx.pm_history_count);
    s_ctx.snapshot.particles_10_0um = (float)(c100 / s_ctx.pm_history_count);
    s_ctx.snapshot.typical_particle_size_um = (float)(tps / s_ctx.pm_history_count);
    s_ctx.snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_ctx.lock);
}

static void sensor_task(void *arg)
{
    int64_t last_scd_check_ms = 0;
    int64_t last_sgp41_check_ms = 0;
    int64_t last_reinit_ms = 0;

    while (s_ctx.running) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (!s_ctx.scd41_initialized || !s_ctx.sgp41_initialized || !s_ctx.sps30_initialized) {
            if (now_ms - last_reinit_ms > 30000) {
                if (!s_ctx.scd41_initialized && sensors_init_scd41() != ESP_OK) {
                    sensors_mark_scd41_invalid();
                    sensors_set_scd41_error("init failed");
                }
                if (!s_ctx.sgp41_initialized && sensors_init_sgp41() != ESP_OK) {
                    sensors_mark_sgp41_invalid();
                    sensors_set_sgp41_error("init failed");
                }
                if (!s_ctx.sps30_initialized && sensors_init_sps30() != ESP_OK) {
                    sensors_mark_sps30_invalid();
                    sensors_set_sps30_error("init failed");
                }
                last_reinit_ms = now_ms;
            }
        }

        if (s_ctx.sps30_initialized && !s_ctx.sps30_sleeping) {
            bool ready = false;
            bool warmup_complete = false;
            bool measurement_ok = false;
            esp_err_t err = ESP_OK;
            sps30_measurement_t measurement = {0};

            /* Serialize sensor I/O so background sampling and control actions do not race on the bus. */
            xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
            if (s_ctx.sps30_initialized && !s_ctx.sps30_sleeping) {
                err = sps30_data_ready(&s_ctx.sps30, &ready);
                if (err == ESP_OK && ready) {
                    warmup_complete = now_ms >= s_ctx.sps30_warmup_until_ms;
                    err = sps30_read_measurement(&s_ctx.sps30, &measurement);
                    measurement_ok = err == ESP_OK;
                }
            }
            xSemaphoreGive(s_ctx.lock);

            if (err == ESP_OK && ready && measurement_ok) {
                if (warmup_complete) {
                    sensors_apply_pm_average(&measurement);
                    sensors_clear_sps30_error();
                }
            } else if (err != ESP_OK) {
                sensors_reset_sps30();
                sensors_set_sps30_error(ready ? "read failed" : "ready check failed");
            }
        }

        if (s_ctx.sgp41_initialized && now_ms - last_sgp41_check_ms >= 1000) {
            uint16_t humidity_ticks = SGP41_DEFAULT_HUMIDITY_TICKS;
            uint16_t temperature_ticks = SGP41_DEFAULT_TEMPERATURE_TICKS;
            last_sgp41_check_ms = now_ms;
            sensors_get_sgp41_compensation_ticks(&humidity_ticks, &temperature_ticks);

            bool conditioning = false;
            bool sample_ok = false;
            esp_err_t err = ESP_OK;

            xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
            if (s_ctx.sgp41_initialized) {
                conditioning = s_ctx.sgp41_conditioning_remaining_s > 0;
                if (conditioning) {
                    uint16_t sraw_voc = 0;
                    err = sgp41_execute_conditioning(&s_ctx.sgp41, humidity_ticks, temperature_ticks, &sraw_voc);
                    if (err == ESP_OK) {
                        s_ctx.snapshot.sgp41_valid = false;
                        s_ctx.snapshot.sgp41_conditioning = true;
                        if (s_ctx.sgp41_conditioning_remaining_s > 0) {
                            s_ctx.sgp41_conditioning_remaining_s--;
                        }
                        sample_ok = true;
                    }
                } else {
                    uint16_t sraw_voc = 0;
                    uint16_t sraw_nox = 0;
                    err = sgp41_measure_raw_signals(&s_ctx.sgp41, humidity_ticks, temperature_ticks, &sraw_voc, &sraw_nox);
                    if (err == ESP_OK) {
                        int32_t voc_index = 0;
                        int32_t nox_index = 0;

                        GasIndexAlgorithm_process(&s_ctx.sgp41_voc_params, sraw_voc, &voc_index);
                        GasIndexAlgorithm_process(&s_ctx.sgp41_nox_params, sraw_nox, &nox_index);

                        s_ctx.snapshot.sgp41_valid = true;
                        s_ctx.snapshot.sgp41_conditioning = false;
                        s_ctx.snapshot.voc_index = voc_index;
                        s_ctx.snapshot.nox_index = nox_index;
                        s_ctx.snapshot.updated_at_ms = now_ms;
                        sample_ok = true;
                    }
                }
            }
            xSemaphoreGive(s_ctx.lock);

            if (sample_ok) {
                sensors_clear_sgp41_error();
            } else if (err != ESP_OK) {
                sensors_reset_sgp41();
                sensors_set_sgp41_error(conditioning ? "conditioning failed" : "read failed");
            }
        }

        if (s_ctx.scd41_initialized && now_ms - last_scd_check_ms >= 1000) {
            bool ready = false;
            last_scd_check_ms = now_ms;
            bool measurement_ok = false;
            esp_err_t err = ESP_OK;

            xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
            if (s_ctx.scd41_initialized) {
                err = scd41_data_ready(&s_ctx.scd41, &ready);
                if (err == ESP_OK && ready) {
                    uint16_t co2 = 0;
                    float temperature = 0.0f;
                    float humidity = 0.0f;
                    err = scd41_read_measurement(&s_ctx.scd41, &co2, &temperature, &humidity);
                    if (err == ESP_OK) {
                        s_ctx.snapshot.scd41_valid = true;
                        s_ctx.snapshot.co2_ppm = co2;
                        s_ctx.snapshot.temperature_c = temperature;
                        s_ctx.snapshot.humidity_rh = humidity;
                        s_ctx.snapshot.updated_at_ms = now_ms;
                        measurement_ok = true;
                    }
                }
            }
            xSemaphoreGive(s_ctx.lock);

            if (measurement_ok) {
                sensors_clear_scd41_error();
            } else if (err != ESP_OK) {
                sensors_reset_scd41();
                sensors_set_scd41_error(ready ? "read failed" : "ready check failed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    s_ctx.task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t sensors_start(const device_config_t *config)
{
    sensors_stop();
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex create failed");
    s_ctx.config = *config;
    s_ctx.running = true;

    if (sensors_init_scd41() != ESP_OK) {
        sensors_mark_scd41_invalid();
        sensors_set_scd41_error("init failed");
    }
    if (sensors_init_sgp41() != ESP_OK) {
        sensors_mark_sgp41_invalid();
        sensors_set_sgp41_error("init failed");
    }
    if (sensors_init_sps30() != ESP_OK) {
        sensors_mark_sps30_invalid();
        sensors_set_sps30_error("init failed");
    }

    BaseType_t created = xTaskCreate(sensor_task, "sensor_task", 6144, NULL, 5, &s_ctx.task_handle);
    if (created != pdPASS) {
        sensors_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sensors_stop(void)
{
    if (s_ctx.running) {
        s_ctx.running = false;
        for (int i = 0; i < 150 && s_ctx.task_handle != NULL; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (s_ctx.task_handle != NULL) {
        ESP_LOGW(TAG, "sensor task did not exit cleanly, forcing delete");
        vTaskDelete(s_ctx.task_handle);
        s_ctx.task_handle = NULL;
    }
    if (s_ctx.scd41_initialized) {
        scd41_stop_periodic_measurement(&s_ctx.scd41);
        scd41_deinit(&s_ctx.scd41);
        s_ctx.scd41_initialized = false;
    }
    if (s_ctx.sgp41_initialized) {
        sgp41_turn_heater_off(&s_ctx.sgp41);
        sgp41_deinit(&s_ctx.sgp41);
        s_ctx.sgp41_initialized = false;
    }
    if (s_ctx.sps30_initialized) {
        sps30_deinit(&s_ctx.sps30);
        s_ctx.sps30_initialized = false;
    }
    if (s_ctx.lock != NULL) {
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}

esp_err_t sensors_get_snapshot(sensor_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot null");
    if (s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    *snapshot = s_ctx.snapshot;
    snapshot->sps30_sleeping = s_ctx.sps30_sleeping;
    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}

bool sensors_any_ready(void)
{
    return s_ctx.scd41_initialized || s_ctx.sgp41_initialized || s_ctx.sps30_initialized;
}

bool sensors_is_scd41_ready(void)
{
    return s_ctx.scd41_initialized;
}

bool sensors_is_sgp41_ready(void)
{
    return s_ctx.sgp41_initialized;
}

bool sensors_is_sps30_ready(void)
{
    return s_ctx.sps30_initialized;
}

bool sensors_is_sps30_sleeping(void)
{
    return s_ctx.sps30_sleeping;
}

void sensors_get_last_error(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    buffer[0] = '\0';
    if (s_ctx.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    strlcpy(buffer, s_ctx.last_error, buffer_len);
    xSemaphoreGive(s_ctx.lock);
}

esp_err_t sensors_set_scd41_asc(bool enabled)
{
    if (!s_ctx.scd41_initialized || s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    esp_err_t err = scd41_stop_periodic_measurement(&s_ctx.scd41);
    if (err == ESP_OK) {
        esp_err_t op_err = scd41_set_automatic_self_calibration(&s_ctx.scd41, enabled);
        esp_err_t resume_err = scd41_start_periodic_measurement(&s_ctx.scd41);
        if (op_err != ESP_OK) {
            err = op_err;
            if (resume_err != ESP_OK) {
                ESP_LOGW(TAG, "failed to resume SCD41 periodic measurement after ASC error: %d", resume_err);
            }
        } else {
            err = resume_err;
        }
    }
    if (err == ESP_OK) {
        s_ctx.config.scd41_asc_enabled = enabled;
        s_ctx.scd41_measurement_started_ms = esp_timer_get_time() / 1000;
    }
    xSemaphoreGive(s_ctx.lock);
    return err;
}

esp_err_t sensors_set_scd41_compensation(uint16_t altitude_m, float temp_offset_c)
{
    if (!s_ctx.scd41_initialized || s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (altitude_m > 3000 || temp_offset_c < 0.0f || temp_offset_c > 20.0f) {
        sensors_set_scd41_error("Compensation values out of range");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    esp_err_t err = scd41_stop_periodic_measurement(&s_ctx.scd41);
    if (err == ESP_OK) {
        esp_err_t temp_err = scd41_set_temperature_offset(&s_ctx.scd41, temp_offset_c);
        esp_err_t alt_err = temp_err == ESP_OK ? scd41_set_sensor_altitude(&s_ctx.scd41, altitude_m) : ESP_OK;
        esp_err_t resume_err = scd41_start_periodic_measurement(&s_ctx.scd41);
        if (temp_err != ESP_OK) {
            err = temp_err;
            if (resume_err != ESP_OK) {
                ESP_LOGW(TAG, "failed to resume SCD41 periodic measurement after temp offset error: %d", resume_err);
            }
        } else if (alt_err != ESP_OK) {
            err = alt_err;
            if (resume_err != ESP_OK) {
                ESP_LOGW(TAG, "failed to resume SCD41 periodic measurement after altitude error: %d", resume_err);
            }
        } else {
            err = resume_err;
        }
    }
    if (err == ESP_OK) {
        s_ctx.config.scd41_altitude_m = altitude_m;
        s_ctx.config.scd41_temp_offset_c = temp_offset_c;
        s_ctx.scd41_measurement_started_ms = esp_timer_get_time() / 1000;
    }
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        sensors_clear_scd41_error();
    } else {
        sensors_set_scd41_error("Compensation update failed");
    }
    return err;
}

esp_err_t sensors_set_scd41_forced_recalibration(uint16_t reference_ppm)
{
    if (!s_ctx.scd41_initialized || s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (reference_ppm < 400 || reference_ppm > 2000) {
        sensors_set_scd41_error("FRC ppm must be 400-2000");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_ctx.scd41_measurement_started_ms == 0 ||
        (now_ms - s_ctx.scd41_measurement_started_ms) < SCD41_FRC_STABILIZATION_MS) {
        xSemaphoreGive(s_ctx.lock);
        sensors_set_scd41_error("FRC requires >=3 min in stable target CO2");
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t correction = 0;
    esp_err_t err = scd41_stop_periodic_measurement(&s_ctx.scd41);
    if (err == ESP_OK) {
        esp_err_t frc_err = scd41_perform_forced_recalibration(&s_ctx.scd41, reference_ppm, &correction);
        esp_err_t resume_err = scd41_start_periodic_measurement(&s_ctx.scd41);
        if (frc_err != ESP_OK) {
            err = frc_err;
            if (resume_err != ESP_OK) {
                ESP_LOGW(TAG, "failed to resume SCD41 periodic measurement after FRC error: %d", resume_err);
            }
        } else {
            err = resume_err;
        }
    }
    if (err == ESP_OK) {
        s_ctx.scd41_measurement_started_ms = esp_timer_get_time() / 1000;
    }
    xSemaphoreGive(s_ctx.lock);
    if (err == ESP_OK) {
        sensors_clear_scd41_error();
    } else {
        sensors_set_scd41_error("FRC failed");
    }
    return err;
}

esp_err_t sensors_set_sps30_sleep(bool sleep)
{
    if (!s_ctx.sps30_initialized || s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    esp_err_t err = sps30_set_sleep(&s_ctx.sps30, sleep);
    if (err == ESP_OK) {
        s_ctx.sps30_sleeping = sleep;
        s_ctx.snapshot.sps30_sleeping = sleep;
        if (sleep) {
            s_ctx.pm_history_count = 0;
            s_ctx.pm_history_index = 0;
            sensors_mark_sps30_invalid_locked();
        } else {
            s_ctx.pm_history_count = 0;
            s_ctx.pm_history_index = 0;
            s_ctx.sps30_warmup_until_ms =
                (esp_timer_get_time() / 1000) + (CONFIG_AIRMON_SPS30_WARMUP_SEC * 1000LL);
            sensors_mark_sps30_invalid_locked();
        }
    }
    xSemaphoreGive(s_ctx.lock);
    return err;
}

esp_err_t sensors_start_sps30_fan_cleaning(void)
{
    if (!s_ctx.sps30_initialized || s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.sps30_sleeping) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = sps30_start_fan_cleaning(&s_ctx.sps30);
    if (err == ESP_OK) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        s_ctx.pm_history_count = 0;
        s_ctx.pm_history_index = 0;
        sensors_mark_sps30_invalid_locked();
        s_ctx.sps30_warmup_until_ms = now_ms + SPS30_FAN_CLEANING_DURATION_MS;
        /* SPS30 does not publish fresh measurements during the 10 s cleaning cycle. */
        s_ctx.sps30.last_measurement_request_ms = now_ms + SPS30_FAN_CLEANING_DURATION_MS - 1000;
    }
    xSemaphoreGive(s_ctx.lock);
    return err;
}
