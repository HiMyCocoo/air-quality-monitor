#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bmp390.h"
#include "device_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "scd41.h"
#include "sensirion_gas_index_algorithm.h"
#include "sgp41.h"
#include "sps30.h"

#define PM_HISTORY_LEN 6
#define BMP390_PRESSURE_HISTORY_LEN 37
#define BMP390_PRESSURE_SAMPLE_INTERVAL_MS (300000LL)
#define BMP390_PRESSURE_HISTORY_MAX_GAP_MS (BMP390_PRESSURE_SAMPLE_INTERVAL_MS * 2LL)
#define BMP390_PRESSURE_TREND_WINDOW_MS (3LL * 60LL * 60LL * 1000LL)
#define BMP390_PRESSURE_TREND_MIN_SPAN_MS (60LL * 60LL * 1000LL)
#define SCD41_HUMIDITY_HISTORY_LEN 37
#define SCD41_HUMIDITY_SAMPLE_INTERVAL_MS (300000LL)
#define SCD41_HUMIDITY_HISTORY_MAX_GAP_MS (SCD41_HUMIDITY_SAMPLE_INTERVAL_MS * 2LL)
#define SCD41_HUMIDITY_TREND_WINDOW_MS (3LL * 60LL * 60LL * 1000LL)
#define SCD41_HUMIDITY_TREND_MIN_SPAN_MS (60LL * 60LL * 60LL * 1000LL)
#define SCD41_FRC_STABILIZATION_MS (180000LL)
#define SCD41_PRESSURE_COMPENSATION_FALLBACK_MS (15000LL)
#define SGP41_CONDITIONING_SEC 10
#define SGP41_VOC_STABILIZATION_MS ((int64_t)((GasIndexAlgorithm_INIT_DURATION_VARIANCE_VOC + GasIndexAlgorithm_INITIAL_BLACKOUT) * 1000.0f))
#define SGP41_NOX_STABILIZATION_MS ((int64_t)((GasIndexAlgorithm_INIT_DURATION_VARIANCE_NOX + GasIndexAlgorithm_INITIAL_BLACKOUT) * 1000.0f))
#define SGP41_DEFAULT_HUMIDITY_TICKS 0x8000
#define SGP41_DEFAULT_TEMPERATURE_TICKS 0x6666
#define SGP41_STATE_NAMESPACE "airmon_sgp41"
#define SGP41_STATE_KEY "checkpoint"
#define SGP41_STATE_VERSION 2
#define SGP41_STATE_SAVE_INTERVAL_MS (300000LL)
#define SPS30_FAN_CLEANING_DURATION_MS (10000LL)
#define TREND_STATE_NAMESPACE "airmon_trend"
#define BMP390_HISTORY_KEY "bmp390_hist"
#define SCD41_HUMIDITY_HISTORY_KEY "scd41_hum"
#define TREND_STATE_VERSION 1
#define TREND_STATE_SAVE_INTERVAL_MS (300000LL)

typedef struct {
    float pressure_hpa;
    int64_t captured_at_ms;
} bmp390_pressure_history_entry_t;

typedef struct {
    float humidity_rh;
    int64_t captured_at_ms;
} scd41_humidity_history_entry_t;

typedef struct {
    volatile bool running;
    volatile bool scd41_initialized;
    volatile bool sgp41_initialized;
    volatile bool bmp390_initialized;
    volatile bool sps30_initialized;
    volatile bool sps30_sleeping;
    TaskHandle_t task_handle;
    SemaphoreHandle_t lock;
    sensor_snapshot_t snapshot;
    char last_error[LAST_ERROR_LEN];
    char scd41_error[LAST_ERROR_LEN];
    char sgp41_error[LAST_ERROR_LEN];
    char bmp390_error[LAST_ERROR_LEN];
    char sps30_error[LAST_ERROR_LEN];
    device_config_t config;
    scd41_t scd41;
    sgp41_t sgp41;
    bmp390_t bmp390;
    sps30_t sps30;
    GasIndexAlgorithmParams sgp41_voc_params;
    GasIndexAlgorithmParams sgp41_nox_params;
    sps30_measurement_t pm_history[PM_HISTORY_LEN];
    bmp390_pressure_history_entry_t bmp390_pressure_history[BMP390_PRESSURE_HISTORY_LEN];
    scd41_humidity_history_entry_t scd41_humidity_history[SCD41_HUMIDITY_HISTORY_LEN];
    size_t pm_history_count;
    size_t pm_history_index;
    size_t bmp390_pressure_history_count;
    size_t bmp390_pressure_history_index;
    size_t scd41_humidity_history_count;
    size_t scd41_humidity_history_index;
    int64_t scd41_measurement_started_ms;
    int64_t sgp41_measurement_started_ms;
    int64_t co2_compensation_updated_at_ms;
    int64_t sps30_warmup_until_ms;
    int64_t sgp41_state_saved_at_ms;
    int64_t bmp390_history_saved_at_ms;
    int64_t scd41_humidity_history_saved_at_ms;
    uint32_t co2_compensation_pressure_pa;
    uint8_t sgp41_conditioning_remaining_s;
    bool sgp41_state_dirty;
    bool bmp390_history_dirty;
    bool scd41_humidity_history_dirty;
} sensors_ctx_t;

extern sensors_ctx_t s_ctx;

typedef struct {
    int32_t algorithm_type;
    float sampling_interval;
    float index_offset;
    int32_t sraw_minimum;
    float gating_max_duration_minutes;
    float init_duration_mean;
    float init_duration_variance;
    float gating_threshold;
    float index_gain;
    float tau_mean_hours;
    float tau_variance_hours;
    float sraw_std_initial;
    float uptime;
    float sraw;
    float gas_index;
    uint8_t mean_variance_estimator_initialized;
    float mean_variance_estimator_mean;
    float mean_variance_estimator_sraw_offset;
    float mean_variance_estimator_std;
    float mean_variance_estimator_gamma_mean_internal;
    float mean_variance_estimator_gamma_variance_internal;
    float mean_variance_estimator_gamma_initial_mean;
    float mean_variance_estimator_gamma_initial_variance;
    float mean_variance_estimator_gamma_mean;
    float mean_variance_estimator_gamma_variance;
    float mean_variance_estimator_uptime_gamma;
    float mean_variance_estimator_uptime_gating;
    float mean_variance_estimator_gating_duration_minutes;
    float mean_variance_estimator_sigmoid_k;
    float mean_variance_estimator_sigmoid_x0;
    float mox_model_sraw_std;
    float mox_model_sraw_mean;
    float sigmoid_scaled_k;
    float sigmoid_scaled_x0;
    float sigmoid_scaled_offset_default;
    float adaptive_lowpass_a1;
    float adaptive_lowpass_a2;
    uint8_t adaptive_lowpass_initialized;
    float adaptive_lowpass_x1;
    float adaptive_lowpass_x2;
    float adaptive_lowpass_x3;
} sgp41_persisted_algorithm_state_t;

typedef struct {
    uint32_t version;
    uint16_t serial_number[3];
    uint32_t measurement_elapsed_s;
    uint8_t conditioning_remaining_s;
    int32_t voc_index;
    int32_t nox_index;
    sgp41_persisted_algorithm_state_t voc_state;
    sgp41_persisted_algorithm_state_t nox_state;
} sgp41_persisted_state_t;

void sensors_deserialize_sgp41_algorithm_state(const sgp41_persisted_algorithm_state_t *state,
                                               GasIndexAlgorithmParams *params);
esp_err_t sensors_load_sgp41_state(sgp41_persisted_state_t *state);
esp_err_t sensors_checkpoint_sgp41_state_internal(bool force);
esp_err_t sensors_load_bmp390_history(int64_t now_ms);
void sensors_checkpoint_bmp390_history(bool force);
esp_err_t sensors_load_scd41_humidity_history(int64_t now_ms);
void sensors_checkpoint_scd41_humidity_history(bool force);
void sensors_clear_bmp390_pressure_history_locked(void);
void sensors_seed_bmp390_pressure_history_locked(float pressure_hpa, int64_t now_ms);
void sensors_clear_scd41_humidity_history_locked(void);
void sensors_seed_scd41_humidity_history_locked(float humidity_rh, int64_t now_ms);
void sensors_update_bmp390_pressure_trend_locked(float pressure_hpa, int64_t now_ms);
void sensors_update_scd41_humidity_trend_locked(float humidity_rh, int64_t now_ms);
