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
#include "pms7003.h"
#include "scd41.h"

#define PMS_HISTORY_LEN 6
#define PMS_WAKE_WARMUP_MS 30000

static const char *TAG = "sensors";

typedef struct {
    bool running;
    bool scd41_initialized;
    bool pms_initialized;
    bool pms_sleeping;
    TaskHandle_t task_handle;
    SemaphoreHandle_t lock;
    sensor_snapshot_t snapshot;
    char last_error[LAST_ERROR_LEN];
    device_config_t config;
    scd41_t scd41;
    pms7003_t pms7003;
    pms7003_frame_t pms_history[PMS_HISTORY_LEN];
    size_t pms_history_count;
    size_t pms_history_index;
    int64_t pms_warmup_until_ms;
} sensors_ctx_t;

static sensors_ctx_t s_ctx;

static void sensors_set_error(const char *message)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    strlcpy(s_ctx.last_error, message, sizeof(s_ctx.last_error));
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_clear_error(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.last_error[0] = '\0';
    xSemaphoreGive(s_ctx.lock);
}

static esp_err_t sensors_init_scd41(void)
{
    if (s_ctx.scd41_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(scd41_init(&s_ctx.scd41,
                                   CONFIG_AIRMON_I2C_PORT_NUM,
                                   CONFIG_AIRMON_I2C_SDA_GPIO,
                                   CONFIG_AIRMON_I2C_SCL_GPIO),
                        TAG, "scd41 init failed");
    ESP_RETURN_ON_ERROR(scd41_stop_periodic_measurement(&s_ctx.scd41), TAG, "scd41 stop failed");
    ESP_RETURN_ON_ERROR(scd41_set_temperature_offset(&s_ctx.scd41, s_ctx.config.scd41_temp_offset_c), TAG, "temp offset failed");
    ESP_RETURN_ON_ERROR(scd41_set_sensor_altitude(&s_ctx.scd41, s_ctx.config.scd41_altitude_m), TAG, "altitude failed");
    ESP_RETURN_ON_ERROR(scd41_set_automatic_self_calibration(&s_ctx.scd41, s_ctx.config.scd41_asc_enabled), TAG, "asc failed");
    ESP_RETURN_ON_ERROR(scd41_start_periodic_measurement(&s_ctx.scd41), TAG, "scd41 start failed");
    s_ctx.scd41_initialized = true;
    return ESP_OK;
}

static esp_err_t sensors_init_pms7003(void)
{
    if (s_ctx.pms_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(pms7003_init(&s_ctx.pms7003,
                                     CONFIG_AIRMON_UART_PORT_NUM,
                                     CONFIG_AIRMON_PMS_TX_GPIO,
                                     CONFIG_AIRMON_PMS_RX_GPIO,
                                     s_ctx.config.pms_control_pins_enabled,
                                     CONFIG_AIRMON_PMS_SET_GPIO,
                                     CONFIG_AIRMON_PMS_RST_GPIO),
                        TAG, "pms init failed");
    s_ctx.pms_initialized = true;
    s_ctx.pms_sleeping = false;
    s_ctx.pms_warmup_until_ms = (esp_timer_get_time() / 1000) + PMS_WAKE_WARMUP_MS;
    return ESP_OK;
}

static void sensors_apply_pms_average(const pms7003_frame_t *frame)
{
    s_ctx.pms_history[s_ctx.pms_history_index] = *frame;
    s_ctx.pms_history_index = (s_ctx.pms_history_index + 1) % PMS_HISTORY_LEN;
    if (s_ctx.pms_history_count < PMS_HISTORY_LEN) {
        s_ctx.pms_history_count++;
    }

    uint32_t pm1 = 0;
    uint32_t pm25 = 0;
    uint32_t pm10 = 0;
    uint32_t c03 = 0;
    uint32_t c05 = 0;
    uint32_t c10 = 0;
    uint32_t c25 = 0;
    uint32_t c50 = 0;
    uint32_t c100 = 0;
    for (size_t i = 0; i < s_ctx.pms_history_count; ++i) {
        pm1 += s_ctx.pms_history[i].pm1_0;
        pm25 += s_ctx.pms_history[i].pm2_5;
        pm10 += s_ctx.pms_history[i].pm10_0;
        c03 += s_ctx.pms_history[i].particles_0_3um;
        c05 += s_ctx.pms_history[i].particles_0_5um;
        c10 += s_ctx.pms_history[i].particles_1_0um;
        c25 += s_ctx.pms_history[i].particles_2_5um;
        c50 += s_ctx.pms_history[i].particles_5_0um;
        c100 += s_ctx.pms_history[i].particles_10_0um;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.snapshot.pms_valid = true;
    s_ctx.snapshot.pm1_0 = pm1 / s_ctx.pms_history_count;
    s_ctx.snapshot.pm2_5 = pm25 / s_ctx.pms_history_count;
    s_ctx.snapshot.pm10_0 = pm10 / s_ctx.pms_history_count;
    s_ctx.snapshot.particles_0_3um = c03 / s_ctx.pms_history_count;
    s_ctx.snapshot.particles_0_5um = c05 / s_ctx.pms_history_count;
    s_ctx.snapshot.particles_1_0um = c10 / s_ctx.pms_history_count;
    s_ctx.snapshot.particles_2_5um = c25 / s_ctx.pms_history_count;
    s_ctx.snapshot.particles_5_0um = c50 / s_ctx.pms_history_count;
    s_ctx.snapshot.particles_10_0um = c100 / s_ctx.pms_history_count;
    s_ctx.snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_ctx.lock);
}

static void sensor_task(void *arg)
{
    int64_t last_scd_check_ms = 0;
    int64_t last_reinit_ms = 0;

    while (s_ctx.running) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (!s_ctx.scd41_initialized || !s_ctx.pms_initialized) {
            if (now_ms - last_reinit_ms > 30000) {
                if (!s_ctx.scd41_initialized && sensors_init_scd41() != ESP_OK) {
                    sensors_set_error("SCD41 init failed");
                }
                if (!s_ctx.pms_initialized && sensors_init_pms7003() != ESP_OK) {
                    sensors_set_error("PMS7003 init failed");
                }
                last_reinit_ms = now_ms;
            }
        }

        if (s_ctx.pms_initialized && !s_ctx.pms_sleeping) {
            pms7003_frame_t frame = {0};
            esp_err_t err = pms7003_poll(&s_ctx.pms7003, &frame, 200);
            if (err == ESP_OK) {
                if (now_ms >= s_ctx.pms_warmup_until_ms) {
                    sensors_apply_pms_average(&frame);
                    sensors_clear_error();
                }
            } else if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_NOT_FOUND) {
                sensors_set_error("PMS7003 read failed");
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (s_ctx.scd41_initialized && now_ms - last_scd_check_ms >= 1000) {
            bool ready = false;
            last_scd_check_ms = now_ms;
            if (scd41_data_ready(&s_ctx.scd41, &ready) == ESP_OK && ready) {
                uint16_t co2 = 0;
                float temperature = 0.0f;
                float humidity = 0.0f;
                if (scd41_read_measurement(&s_ctx.scd41, &co2, &temperature, &humidity) == ESP_OK) {
                    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
                    s_ctx.snapshot.scd41_valid = true;
                    s_ctx.snapshot.co2_ppm = co2;
                    s_ctx.snapshot.temperature_c = temperature;
                    s_ctx.snapshot.humidity_rh = humidity;
                    s_ctx.snapshot.updated_at_ms = now_ms;
                    xSemaphoreGive(s_ctx.lock);
                    sensors_clear_error();
                } else {
                    sensors_set_error("SCD41 read failed");
                }
            }
        }
    }

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
        sensors_set_error("SCD41 init failed");
    }
    if (sensors_init_pms7003() != ESP_OK) {
        sensors_set_error("PMS7003 init failed");
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
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_ctx.scd41_initialized) {
        scd41_stop_periodic_measurement(&s_ctx.scd41);
        scd41_deinit(&s_ctx.scd41);
        s_ctx.scd41_initialized = false;
    }
    if (s_ctx.pms_initialized) {
        pms7003_deinit(&s_ctx.pms7003);
        s_ctx.pms_initialized = false;
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
    snapshot->pms_sleeping = s_ctx.pms_sleeping;
    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}

bool sensors_is_ready(void)
{
    return s_ctx.scd41_initialized && s_ctx.pms_initialized;
}

bool sensors_is_pms_sleeping(void)
{
    return s_ctx.pms_sleeping;
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
        err = scd41_set_automatic_self_calibration(&s_ctx.scd41, enabled);
    }
    if (err == ESP_OK) {
        err = scd41_start_periodic_measurement(&s_ctx.scd41);
    }
    if (err == ESP_OK) {
        s_ctx.config.scd41_asc_enabled = enabled;
    }
    xSemaphoreGive(s_ctx.lock);
    return err;
}

esp_err_t sensors_set_scd41_forced_recalibration(uint16_t reference_ppm)
{
    if (!s_ctx.scd41_initialized || s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (reference_ppm < 400 || reference_ppm > 2000) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    uint16_t correction = 0;
    esp_err_t err = scd41_stop_periodic_measurement(&s_ctx.scd41);
    if (err == ESP_OK) {
        err = scd41_perform_forced_recalibration(&s_ctx.scd41, reference_ppm, &correction);
    }
    if (err == ESP_OK) {
        err = scd41_start_periodic_measurement(&s_ctx.scd41);
    }
    xSemaphoreGive(s_ctx.lock);
    return err;
}

esp_err_t sensors_set_pms_sleep(bool sleep)
{
    if (!s_ctx.pms_initialized || s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    esp_err_t err = pms7003_set_sleep(&s_ctx.pms7003, sleep);
    if (err == ESP_OK) {
        s_ctx.pms_sleeping = sleep;
        s_ctx.snapshot.pms_sleeping = sleep;
        if (!sleep) {
            s_ctx.pms_history_count = 0;
            s_ctx.pms_history_index = 0;
            s_ctx.pms_warmup_until_ms = (esp_timer_get_time() / 1000) + PMS_WAKE_WARMUP_MS;
            s_ctx.snapshot.pms_valid = false;
        }
    }
    xSemaphoreGive(s_ctx.lock);
    return err;
}
