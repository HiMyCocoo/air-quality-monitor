#include "sensors.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bmp390.h"
#include "scd41.h"
#include "sensirion_gas_index_algorithm.h"
#include "sgp41.h"
#include "sps30.h"

#define PM_HISTORY_LEN 6
#define BMP390_PRESSURE_HISTORY_LEN 37
#define BMP390_PRESSURE_SAMPLE_INTERVAL_MS (300000LL)
#define BMP390_PRESSURE_TREND_WINDOW_MS (3LL * 60LL * 60LL * 1000LL)
#define BMP390_PRESSURE_TREND_MIN_SPAN_MS (60LL * 60LL * 1000LL)
#define SCD41_HUMIDITY_HISTORY_LEN 37
#define SCD41_HUMIDITY_SAMPLE_INTERVAL_MS (300000LL)
#define SCD41_HUMIDITY_TREND_WINDOW_MS (3LL * 60LL * 60LL * 1000LL)
#define SCD41_HUMIDITY_TREND_MIN_SPAN_MS (60LL * 60LL * 1000LL)
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

static const char *TAG = "sensors";

typedef struct {
    bool running;
    bool scd41_initialized;
    bool sgp41_initialized;
    bool bmp390_initialized;
    bool sps30_initialized;
    bool sps30_sleeping;
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
    struct {
        float pressure_hpa;
        int64_t captured_at_ms;
    } bmp390_pressure_history[BMP390_PRESSURE_HISTORY_LEN];
    struct {
        float humidity_rh;
        int64_t captured_at_ms;
    } scd41_humidity_history[SCD41_HUMIDITY_HISTORY_LEN];
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

static sensors_ctx_t s_ctx;

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

static void sensors_serialize_sgp41_algorithm_state(const GasIndexAlgorithmParams *params,
                                                    sgp41_persisted_algorithm_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->algorithm_type = params->mAlgorithm_Type;
    state->sampling_interval = params->mSamplingInterval;
    state->index_offset = params->mIndex_Offset;
    state->sraw_minimum = params->mSraw_Minimum;
    state->gating_max_duration_minutes = params->mGating_Max_Duration_Minutes;
    state->init_duration_mean = params->mInit_Duration_Mean;
    state->init_duration_variance = params->mInit_Duration_Variance;
    state->gating_threshold = params->mGating_Threshold;
    state->index_gain = params->mIndex_Gain;
    state->tau_mean_hours = params->mTau_Mean_Hours;
    state->tau_variance_hours = params->mTau_Variance_Hours;
    state->sraw_std_initial = params->mSraw_Std_Initial;
    state->uptime = params->mUptime;
    state->sraw = params->mSraw;
    state->gas_index = params->mGas_Index;
    state->mean_variance_estimator_initialized = params->m_Mean_Variance_Estimator___Initialized ? 1U : 0U;
    state->mean_variance_estimator_mean = params->m_Mean_Variance_Estimator___Mean;
    state->mean_variance_estimator_sraw_offset = params->m_Mean_Variance_Estimator___Sraw_Offset;
    state->mean_variance_estimator_std = params->m_Mean_Variance_Estimator___Std;
    state->mean_variance_estimator_gamma_mean_internal = params->m_Mean_Variance_Estimator___Gamma_Mean;
    state->mean_variance_estimator_gamma_variance_internal = params->m_Mean_Variance_Estimator___Gamma_Variance;
    state->mean_variance_estimator_gamma_initial_mean = params->m_Mean_Variance_Estimator___Gamma_Initial_Mean;
    state->mean_variance_estimator_gamma_initial_variance = params->m_Mean_Variance_Estimator___Gamma_Initial_Variance;
    state->mean_variance_estimator_gamma_mean = params->m_Mean_Variance_Estimator__Gamma_Mean;
    state->mean_variance_estimator_gamma_variance = params->m_Mean_Variance_Estimator__Gamma_Variance;
    state->mean_variance_estimator_uptime_gamma = params->m_Mean_Variance_Estimator___Uptime_Gamma;
    state->mean_variance_estimator_uptime_gating = params->m_Mean_Variance_Estimator___Uptime_Gating;
    state->mean_variance_estimator_gating_duration_minutes = params->m_Mean_Variance_Estimator___Gating_Duration_Minutes;
    state->mean_variance_estimator_sigmoid_k = params->m_Mean_Variance_Estimator___Sigmoid__K;
    state->mean_variance_estimator_sigmoid_x0 = params->m_Mean_Variance_Estimator___Sigmoid__X0;
    state->mox_model_sraw_std = params->m_Mox_Model__Sraw_Std;
    state->mox_model_sraw_mean = params->m_Mox_Model__Sraw_Mean;
    state->sigmoid_scaled_k = params->m_Sigmoid_Scaled__K;
    state->sigmoid_scaled_x0 = params->m_Sigmoid_Scaled__X0;
    state->sigmoid_scaled_offset_default = params->m_Sigmoid_Scaled__Offset_Default;
    state->adaptive_lowpass_a1 = params->m_Adaptive_Lowpass__A1;
    state->adaptive_lowpass_a2 = params->m_Adaptive_Lowpass__A2;
    state->adaptive_lowpass_initialized = params->m_Adaptive_Lowpass___Initialized ? 1U : 0U;
    state->adaptive_lowpass_x1 = params->m_Adaptive_Lowpass___X1;
    state->adaptive_lowpass_x2 = params->m_Adaptive_Lowpass___X2;
    state->adaptive_lowpass_x3 = params->m_Adaptive_Lowpass___X3;
}

static void sensors_deserialize_sgp41_algorithm_state(const sgp41_persisted_algorithm_state_t *state,
                                                      GasIndexAlgorithmParams *params)
{
    memset(params, 0, sizeof(*params));
    params->mAlgorithm_Type = state->algorithm_type;
    params->mSamplingInterval = state->sampling_interval;
    params->mIndex_Offset = state->index_offset;
    params->mSraw_Minimum = state->sraw_minimum;
    params->mGating_Max_Duration_Minutes = state->gating_max_duration_minutes;
    params->mInit_Duration_Mean = state->init_duration_mean;
    params->mInit_Duration_Variance = state->init_duration_variance;
    params->mGating_Threshold = state->gating_threshold;
    params->mIndex_Gain = state->index_gain;
    params->mTau_Mean_Hours = state->tau_mean_hours;
    params->mTau_Variance_Hours = state->tau_variance_hours;
    params->mSraw_Std_Initial = state->sraw_std_initial;
    params->mUptime = state->uptime;
    params->mSraw = state->sraw;
    params->mGas_Index = state->gas_index;
    params->m_Mean_Variance_Estimator___Initialized = state->mean_variance_estimator_initialized != 0U;
    params->m_Mean_Variance_Estimator___Mean = state->mean_variance_estimator_mean;
    params->m_Mean_Variance_Estimator___Sraw_Offset = state->mean_variance_estimator_sraw_offset;
    params->m_Mean_Variance_Estimator___Std = state->mean_variance_estimator_std;
    params->m_Mean_Variance_Estimator___Gamma_Mean = state->mean_variance_estimator_gamma_mean_internal;
    params->m_Mean_Variance_Estimator___Gamma_Variance = state->mean_variance_estimator_gamma_variance_internal;
    params->m_Mean_Variance_Estimator___Gamma_Initial_Mean = state->mean_variance_estimator_gamma_initial_mean;
    params->m_Mean_Variance_Estimator___Gamma_Initial_Variance = state->mean_variance_estimator_gamma_initial_variance;
    params->m_Mean_Variance_Estimator__Gamma_Mean = state->mean_variance_estimator_gamma_mean;
    params->m_Mean_Variance_Estimator__Gamma_Variance = state->mean_variance_estimator_gamma_variance;
    params->m_Mean_Variance_Estimator___Uptime_Gamma = state->mean_variance_estimator_uptime_gamma;
    params->m_Mean_Variance_Estimator___Uptime_Gating = state->mean_variance_estimator_uptime_gating;
    params->m_Mean_Variance_Estimator___Gating_Duration_Minutes = state->mean_variance_estimator_gating_duration_minutes;
    params->m_Mean_Variance_Estimator___Sigmoid__K = state->mean_variance_estimator_sigmoid_k;
    params->m_Mean_Variance_Estimator___Sigmoid__X0 = state->mean_variance_estimator_sigmoid_x0;
    params->m_Mox_Model__Sraw_Std = state->mox_model_sraw_std;
    params->m_Mox_Model__Sraw_Mean = state->mox_model_sraw_mean;
    params->m_Sigmoid_Scaled__K = state->sigmoid_scaled_k;
    params->m_Sigmoid_Scaled__X0 = state->sigmoid_scaled_x0;
    params->m_Sigmoid_Scaled__Offset_Default = state->sigmoid_scaled_offset_default;
    params->m_Adaptive_Lowpass__A1 = state->adaptive_lowpass_a1;
    params->m_Adaptive_Lowpass__A2 = state->adaptive_lowpass_a2;
    params->m_Adaptive_Lowpass___Initialized = state->adaptive_lowpass_initialized != 0U;
    params->m_Adaptive_Lowpass___X1 = state->adaptive_lowpass_x1;
    params->m_Adaptive_Lowpass___X2 = state->adaptive_lowpass_x2;
    params->m_Adaptive_Lowpass___X3 = state->adaptive_lowpass_x3;
}

static bool sensors_sgp41_state_look_valid(const sgp41_persisted_algorithm_state_t *state, int expected_type)
{
    return state->algorithm_type == expected_type &&
           isfinite(state->sampling_interval) &&
           state->sampling_interval > 0.0f &&
           isfinite(state->uptime) &&
           state->uptime >= 0.0f;
}

static esp_err_t sensors_load_sgp41_state(sgp41_persisted_state_t *state)
{
    nvs_handle_t handle;
    size_t required = sizeof(*state);
    esp_err_t err = nvs_open(SGP41_STATE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, SGP41_STATE_KEY, state, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (required != sizeof(*state) ||
        state->version != SGP41_STATE_VERSION ||
        !sensors_sgp41_state_look_valid(&state->voc_state, GasIndexAlgorithm_ALGORITHM_TYPE_VOC) ||
        !sensors_sgp41_state_look_valid(&state->nox_state, GasIndexAlgorithm_ALGORITHM_TYPE_NOX)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t sensors_store_sgp41_state(const sgp41_persisted_state_t *state)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SGP41_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, SGP41_STATE_KEY, state, sizeof(*state));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool sensors_prepare_sgp41_checkpoint_locked(sgp41_persisted_state_t *state, int64_t now_ms, bool force)
{
    if (!s_ctx.sgp41_initialized || s_ctx.sgp41_measurement_started_ms == 0) {
        return false;
    }
    if (!force && (!s_ctx.sgp41_state_dirty || (now_ms - s_ctx.sgp41_state_saved_at_ms) < SGP41_STATE_SAVE_INTERVAL_MS)) {
        return false;
    }

    int64_t elapsed_ms = now_ms - s_ctx.sgp41_measurement_started_ms;
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }

    memset(state, 0, sizeof(*state));
    state->version = SGP41_STATE_VERSION;
    state->measurement_elapsed_s = (uint32_t)(elapsed_ms / 1000);
    state->conditioning_remaining_s = s_ctx.sgp41_conditioning_remaining_s;
    state->voc_index = s_ctx.snapshot.voc_index;
    state->nox_index = s_ctx.snapshot.nox_index;
    sensors_serialize_sgp41_algorithm_state(&s_ctx.sgp41_voc_params, &state->voc_state);
    sensors_serialize_sgp41_algorithm_state(&s_ctx.sgp41_nox_params, &state->nox_state);
    return sgp41_get_serial_number(&s_ctx.sgp41, state->serial_number) == ESP_OK;
}

static esp_err_t sensors_checkpoint_sgp41_state_internal(bool force)
{
    if (s_ctx.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sgp41_persisted_state_t state = {0};
    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    bool should_save = sensors_prepare_sgp41_checkpoint_locked(&state, now_ms, force);
    xSemaphoreGive(s_ctx.lock);

    if (!should_save) {
        return ESP_OK;
    }

    esp_err_t err = sensors_store_sgp41_state(&state);
    if (err == ESP_OK) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        s_ctx.sgp41_state_dirty = false;
        s_ctx.sgp41_state_saved_at_ms = now_ms;
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGI(TAG,
                 "SGP41 checkpoint saved: elapsed=%" PRIu32 "s conditioning=%u voc=%" PRId32 " nox=%" PRId32,
                 state.measurement_elapsed_s,
                 state.conditioning_remaining_s,
                 state.voc_index,
                 state.nox_index);
    }
    return err;
}

/* ── BMP390 pressure history persistence ────────────────────────────── */

typedef struct {
    int64_t offset_ms;   /* milliseconds before save-time */
    float   pressure_hpa;
} bmp390_persisted_entry_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t write_index;
    bmp390_persisted_entry_t entries[BMP390_PRESSURE_HISTORY_LEN];
} bmp390_persisted_history_t;

static esp_err_t sensors_store_bmp390_history(int64_t now_ms)
{
    bmp390_persisted_history_t persisted = {0};
    persisted.version = TREND_STATE_VERSION;
    persisted.count   = (uint32_t)s_ctx.bmp390_pressure_history_count;
    persisted.write_index = (uint32_t)s_ctx.bmp390_pressure_history_index;

    for (size_t i = 0; i < s_ctx.bmp390_pressure_history_count; ++i) {
        persisted.entries[i].pressure_hpa = s_ctx.bmp390_pressure_history[i].pressure_hpa;
        persisted.entries[i].offset_ms    = now_ms - s_ctx.bmp390_pressure_history[i].captured_at_ms;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TREND_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, BMP390_HISTORY_KEY, &persisted, sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t sensors_load_bmp390_history(int64_t now_ms)
{
    bmp390_persisted_history_t persisted = {0};
    size_t required = sizeof(persisted);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TREND_STATE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(handle, BMP390_HISTORY_KEY, &persisted, &required);
    nvs_close(handle);
    if (err != ESP_OK || required != sizeof(persisted)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (persisted.version != TREND_STATE_VERSION ||
        persisted.count > BMP390_PRESSURE_HISTORY_LEN ||
        persisted.write_index >= BMP390_PRESSURE_HISTORY_LEN) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < persisted.count; ++i) {
        if (!isfinite(persisted.entries[i].pressure_hpa) ||
            persisted.entries[i].pressure_hpa <= 0.0f ||
            persisted.entries[i].offset_ms < 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_ctx.bmp390_pressure_history_count = persisted.count;
    s_ctx.bmp390_pressure_history_index = persisted.write_index;
    for (size_t i = 0; i < persisted.count; ++i) {
        s_ctx.bmp390_pressure_history[i].pressure_hpa = persisted.entries[i].pressure_hpa;
        s_ctx.bmp390_pressure_history[i].captured_at_ms = now_ms - persisted.entries[i].offset_ms;
    }

    ESP_LOGI(TAG, "BMP390 pressure history restored: %u samples", (unsigned)persisted.count);
    return ESP_OK;
}

static void sensors_checkpoint_bmp390_history(bool force)
{
    if (s_ctx.lock == NULL) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    bool should_save = s_ctx.bmp390_history_dirty &&
                       (force || (now_ms - s_ctx.bmp390_history_saved_at_ms) >= TREND_STATE_SAVE_INTERVAL_MS);
    if (!should_save || s_ctx.bmp390_pressure_history_count == 0) {
        xSemaphoreGive(s_ctx.lock);
        return;
    }

    esp_err_t err = sensors_store_bmp390_history(now_ms);
    if (err == ESP_OK) {
        s_ctx.bmp390_history_dirty = false;
        s_ctx.bmp390_history_saved_at_ms = now_ms;
    }
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BMP390 pressure history checkpoint saved: %u samples",
                 (unsigned)s_ctx.bmp390_pressure_history_count);
    }
}

/* ── SCD41 humidity history persistence ─────────────────────────────── */

typedef struct {
    int64_t offset_ms;
    float   humidity_rh;
} scd41_persisted_humidity_entry_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t write_index;
    scd41_persisted_humidity_entry_t entries[SCD41_HUMIDITY_HISTORY_LEN];
} scd41_persisted_humidity_history_t;

static esp_err_t sensors_store_scd41_humidity_history(int64_t now_ms)
{
    scd41_persisted_humidity_history_t persisted = {0};
    persisted.version     = TREND_STATE_VERSION;
    persisted.count       = (uint32_t)s_ctx.scd41_humidity_history_count;
    persisted.write_index = (uint32_t)s_ctx.scd41_humidity_history_index;

    for (size_t i = 0; i < s_ctx.scd41_humidity_history_count; ++i) {
        persisted.entries[i].humidity_rh = s_ctx.scd41_humidity_history[i].humidity_rh;
        persisted.entries[i].offset_ms   = now_ms - s_ctx.scd41_humidity_history[i].captured_at_ms;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TREND_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, SCD41_HUMIDITY_HISTORY_KEY, &persisted, sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t sensors_load_scd41_humidity_history(int64_t now_ms)
{
    scd41_persisted_humidity_history_t persisted = {0};
    size_t required = sizeof(persisted);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TREND_STATE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(handle, SCD41_HUMIDITY_HISTORY_KEY, &persisted, &required);
    nvs_close(handle);
    if (err != ESP_OK || required != sizeof(persisted)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (persisted.version != TREND_STATE_VERSION ||
        persisted.count > SCD41_HUMIDITY_HISTORY_LEN ||
        persisted.write_index >= SCD41_HUMIDITY_HISTORY_LEN) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < persisted.count; ++i) {
        if (!isfinite(persisted.entries[i].humidity_rh) ||
            persisted.entries[i].humidity_rh < 0.0f ||
            persisted.entries[i].humidity_rh > 100.0f ||
            persisted.entries[i].offset_ms < 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_ctx.scd41_humidity_history_count = persisted.count;
    s_ctx.scd41_humidity_history_index = persisted.write_index;
    for (size_t i = 0; i < persisted.count; ++i) {
        s_ctx.scd41_humidity_history[i].humidity_rh    = persisted.entries[i].humidity_rh;
        s_ctx.scd41_humidity_history[i].captured_at_ms = now_ms - persisted.entries[i].offset_ms;
    }

    ESP_LOGI(TAG, "SCD41 humidity history restored: %u samples", (unsigned)persisted.count);
    return ESP_OK;
}

static void sensors_checkpoint_scd41_humidity_history(bool force)
{
    if (s_ctx.lock == NULL) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    bool should_save = s_ctx.scd41_humidity_history_dirty &&
                       (force || (now_ms - s_ctx.scd41_humidity_history_saved_at_ms) >= TREND_STATE_SAVE_INTERVAL_MS);
    if (!should_save || s_ctx.scd41_humidity_history_count == 0) {
        xSemaphoreGive(s_ctx.lock);
        return;
    }

    esp_err_t err = sensors_store_scd41_humidity_history(now_ms);
    if (err == ESP_OK) {
        s_ctx.scd41_humidity_history_dirty = false;
        s_ctx.scd41_humidity_history_saved_at_ms = now_ms;
    }
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SCD41 humidity history checkpoint saved: %u samples",
                 (unsigned)s_ctx.scd41_humidity_history_count);
    }
}

static void sensors_clear_co2_compensation_locked(void)
{
    s_ctx.snapshot.co2_compensation_source = CO2_COMPENSATION_SOURCE_NONE;
    s_ctx.co2_compensation_pressure_pa = 0;
    s_ctx.co2_compensation_updated_at_ms = 0;
}

static void sensors_set_static_co2_compensation_locked(void)
{
    s_ctx.snapshot.co2_compensation_source = CO2_COMPENSATION_SOURCE_ALTITUDE;
    s_ctx.co2_compensation_pressure_pa = 0;
    s_ctx.co2_compensation_updated_at_ms = 0;
}

static void sensors_set_bmp390_co2_compensation_locked(uint32_t pressure_pa, int64_t now_ms)
{
    s_ctx.snapshot.co2_compensation_source = CO2_COMPENSATION_SOURCE_BMP390;
    s_ctx.co2_compensation_pressure_pa = pressure_pa;
    s_ctx.co2_compensation_updated_at_ms = now_ms;
}

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
    if (s_ctx.bmp390_error[0] != '\0') {
        if (s_ctx.last_error[0] != '\0') {
            strlcat(s_ctx.last_error, " | ", sizeof(s_ctx.last_error));
        }
        strlcat(s_ctx.last_error, "BMP390: ", sizeof(s_ctx.last_error));
        strlcat(s_ctx.last_error, s_ctx.bmp390_error, sizeof(s_ctx.last_error));
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
    sensors_clear_co2_compensation_locked();
    s_ctx.snapshot.co2_ppm = 0;
    s_ctx.snapshot.temperature_c = 0.0f;
    s_ctx.snapshot.humidity_rh = 0.0f;
    s_ctx.snapshot.humidity_trend_valid = false;
    s_ctx.snapshot.humidity_trend_rh_3h = 0.0f;
    s_ctx.snapshot.humidity_trend_span_min = 0;
    s_ctx.scd41_humidity_history_count = 0;
    s_ctx.scd41_humidity_history_index = 0;
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

static void sensors_mark_bmp390_invalid_locked(void)
{
    s_ctx.snapshot.bmp390_valid = false;
    s_ctx.snapshot.bmp390_temperature_c = 0.0f;
    s_ctx.snapshot.pressure_hpa = 0.0f;
    s_ctx.snapshot.pressure_trend_valid = false;
    s_ctx.snapshot.pressure_trend_hpa_3h = 0.0f;
    s_ctx.snapshot.pressure_trend_span_min = 0;
    s_ctx.bmp390_pressure_history_count = 0;
    s_ctx.bmp390_pressure_history_index = 0;
}

static void sensors_mark_sgp41_invalid_locked(void)
{
    s_ctx.snapshot.sgp41_valid = false;
    s_ctx.snapshot.sgp41_conditioning = false;
    s_ctx.snapshot.sgp41_voc_valid = false;
    s_ctx.snapshot.sgp41_nox_valid = false;
    s_ctx.snapshot.sgp41_voc_stabilization_remaining_s = 0;
    s_ctx.snapshot.sgp41_nox_stabilization_remaining_s = 0;
    s_ctx.snapshot.voc_index = 0;
    s_ctx.snapshot.nox_index = 0;
}

static void sensors_update_bmp390_pressure_trend_locked(float pressure_hpa, int64_t now_ms)
{
    s_ctx.snapshot.pressure_trend_valid = false;
    s_ctx.snapshot.pressure_trend_hpa_3h = 0.0f;
    s_ctx.snapshot.pressure_trend_span_min = 0;

    if (!isfinite(pressure_hpa) || pressure_hpa <= 0.0f) {
        return;
    }

    if (s_ctx.bmp390_pressure_history_count == 0) {
        s_ctx.bmp390_pressure_history[0].pressure_hpa = pressure_hpa;
        s_ctx.bmp390_pressure_history[0].captured_at_ms = now_ms;
        s_ctx.bmp390_pressure_history_count = 1;
        s_ctx.bmp390_pressure_history_index = 1 % BMP390_PRESSURE_HISTORY_LEN;
        return;
    }

    size_t newest_index =
        (s_ctx.bmp390_pressure_history_index + BMP390_PRESSURE_HISTORY_LEN - 1) % BMP390_PRESSURE_HISTORY_LEN;
    int64_t newest_at_ms = s_ctx.bmp390_pressure_history[newest_index].captured_at_ms;
    if ((now_ms - newest_at_ms) >= BMP390_PRESSURE_SAMPLE_INTERVAL_MS) {
        size_t write_index = s_ctx.bmp390_pressure_history_index;
        s_ctx.bmp390_pressure_history[write_index].pressure_hpa = pressure_hpa;
        s_ctx.bmp390_pressure_history[write_index].captured_at_ms = now_ms;
        s_ctx.bmp390_pressure_history_index = (write_index + 1) % BMP390_PRESSURE_HISTORY_LEN;
        if (s_ctx.bmp390_pressure_history_count < BMP390_PRESSURE_HISTORY_LEN) {
            s_ctx.bmp390_pressure_history_count++;
        }
        s_ctx.bmp390_history_dirty = true;
    }

    size_t oldest_index = s_ctx.bmp390_pressure_history_count == BMP390_PRESSURE_HISTORY_LEN
                              ? s_ctx.bmp390_pressure_history_index
                              : 0;
    int64_t window_start_ms = now_ms - BMP390_PRESSURE_TREND_WINDOW_MS;
    float baseline_pressure_hpa = 0.0f;
    int64_t baseline_at_ms = 0;
    bool baseline_found = false;

    for (size_t i = 0; i < s_ctx.bmp390_pressure_history_count; ++i) {
        size_t index = (oldest_index + i) % BMP390_PRESSURE_HISTORY_LEN;
        float candidate_pressure_hpa = s_ctx.bmp390_pressure_history[index].pressure_hpa;
        int64_t candidate_at_ms = s_ctx.bmp390_pressure_history[index].captured_at_ms;
        if (!isfinite(candidate_pressure_hpa) || candidate_pressure_hpa <= 0.0f || candidate_at_ms <= 0) {
            continue;
        }
        if (candidate_at_ms < window_start_ms) {
            continue;
        }
        baseline_pressure_hpa = candidate_pressure_hpa;
        baseline_at_ms = candidate_at_ms;
        baseline_found = true;
        break;
    }

    if (!baseline_found) {
        return;
    }

    int64_t span_ms = now_ms - baseline_at_ms;
    if (span_ms < BMP390_PRESSURE_TREND_MIN_SPAN_MS) {
        return;
    }

    s_ctx.snapshot.pressure_trend_valid = true;
    s_ctx.snapshot.pressure_trend_hpa_3h =
        (float)((pressure_hpa - baseline_pressure_hpa) *
                ((double)BMP390_PRESSURE_TREND_WINDOW_MS / (double)span_ms));
    s_ctx.snapshot.pressure_trend_span_min = (uint16_t)((span_ms + 30000LL) / 60000LL);
}

static void sensors_update_scd41_humidity_trend_locked(float humidity_rh, int64_t now_ms)
{
    s_ctx.snapshot.humidity_trend_valid = false;
    s_ctx.snapshot.humidity_trend_rh_3h = 0.0f;
    s_ctx.snapshot.humidity_trend_span_min = 0;

    if (!isfinite(humidity_rh) || humidity_rh < 0.0f || humidity_rh > 100.0f) {
        return;
    }

    if (s_ctx.scd41_humidity_history_count == 0) {
        s_ctx.scd41_humidity_history[0].humidity_rh = humidity_rh;
        s_ctx.scd41_humidity_history[0].captured_at_ms = now_ms;
        s_ctx.scd41_humidity_history_count = 1;
        s_ctx.scd41_humidity_history_index = 1 % SCD41_HUMIDITY_HISTORY_LEN;
        return;
    }

    size_t newest_index =
        (s_ctx.scd41_humidity_history_index + SCD41_HUMIDITY_HISTORY_LEN - 1) % SCD41_HUMIDITY_HISTORY_LEN;
    int64_t newest_at_ms = s_ctx.scd41_humidity_history[newest_index].captured_at_ms;
    if ((now_ms - newest_at_ms) >= SCD41_HUMIDITY_SAMPLE_INTERVAL_MS) {
        size_t write_index = s_ctx.scd41_humidity_history_index;
        s_ctx.scd41_humidity_history[write_index].humidity_rh = humidity_rh;
        s_ctx.scd41_humidity_history[write_index].captured_at_ms = now_ms;
        s_ctx.scd41_humidity_history_index = (write_index + 1) % SCD41_HUMIDITY_HISTORY_LEN;
        if (s_ctx.scd41_humidity_history_count < SCD41_HUMIDITY_HISTORY_LEN) {
            s_ctx.scd41_humidity_history_count++;
        }
        s_ctx.scd41_humidity_history_dirty = true;
    }

    size_t oldest_index = s_ctx.scd41_humidity_history_count == SCD41_HUMIDITY_HISTORY_LEN
                              ? s_ctx.scd41_humidity_history_index
                              : 0;
    int64_t window_start_ms = now_ms - SCD41_HUMIDITY_TREND_WINDOW_MS;
    float baseline_humidity_rh = 0.0f;
    int64_t baseline_at_ms = 0;
    bool baseline_found = false;

    for (size_t i = 0; i < s_ctx.scd41_humidity_history_count; ++i) {
        size_t index = (oldest_index + i) % SCD41_HUMIDITY_HISTORY_LEN;
        float candidate_humidity_rh = s_ctx.scd41_humidity_history[index].humidity_rh;
        int64_t candidate_at_ms = s_ctx.scd41_humidity_history[index].captured_at_ms;
        if (!isfinite(candidate_humidity_rh) || candidate_humidity_rh < 0.0f || candidate_humidity_rh > 100.0f ||
            candidate_at_ms <= 0) {
            continue;
        }
        if (candidate_at_ms < window_start_ms) {
            continue;
        }
        baseline_humidity_rh = candidate_humidity_rh;
        baseline_at_ms = candidate_at_ms;
        baseline_found = true;
        break;
    }

    if (!baseline_found) {
        return;
    }

    int64_t span_ms = now_ms - baseline_at_ms;
    if (span_ms < SCD41_HUMIDITY_TREND_MIN_SPAN_MS) {
        return;
    }

    s_ctx.snapshot.humidity_trend_valid = true;
    s_ctx.snapshot.humidity_trend_rh_3h =
        (float)((humidity_rh - baseline_humidity_rh) *
                ((double)SCD41_HUMIDITY_TREND_WINDOW_MS / (double)span_ms));
    s_ctx.snapshot.humidity_trend_span_min = (uint16_t)((span_ms + 30000LL) / 60000LL);
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

static void sensors_set_bmp390_error(const char *message)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    strlcpy(s_ctx.bmp390_error, message, sizeof(s_ctx.bmp390_error));
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

static void sensors_clear_bmp390_error(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.bmp390_error[0] = '\0';
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

static void sensors_mark_bmp390_invalid(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    sensors_mark_bmp390_invalid_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_mark_sgp41_invalid(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    sensors_mark_sgp41_invalid_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_deinit_scd41_locked(void)
{
    if (s_ctx.scd41.dev_handle != NULL || s_ctx.scd41.bus_handle != NULL) {
        scd41_deinit(&s_ctx.scd41);
    }
    memset(&s_ctx.scd41, 0, sizeof(s_ctx.scd41));
    s_ctx.scd41_initialized = false;
    s_ctx.scd41_measurement_started_ms = 0;
    sensors_mark_scd41_invalid_locked();
}

static void sensors_deinit_sgp41_locked(void)
{
    if (s_ctx.sgp41.dev_handle != NULL || s_ctx.sgp41.bus_handle != NULL) {
        sgp41_deinit(&s_ctx.sgp41);
    }
    memset(&s_ctx.sgp41, 0, sizeof(s_ctx.sgp41));
    s_ctx.sgp41_initialized = false;
    s_ctx.sgp41_conditioning_remaining_s = 0;
    s_ctx.sgp41_measurement_started_ms = 0;
    sensors_mark_sgp41_invalid_locked();
}

static void sensors_deinit_bmp390_locked(void)
{
    if (s_ctx.bmp390.dev_handle != NULL || s_ctx.bmp390.bus_handle != NULL) {
        bmp390_deinit(&s_ctx.bmp390);
    }
    memset(&s_ctx.bmp390, 0, sizeof(s_ctx.bmp390));
    s_ctx.bmp390_initialized = false;
    sensors_mark_bmp390_invalid_locked();
}

static void sensors_reset_scd41(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.scd41.owns_bus) {
        if (s_ctx.sgp41_initialized && !s_ctx.sgp41.owns_bus && s_ctx.sgp41.bus_handle == s_ctx.scd41.bus_handle) {
            sensors_deinit_sgp41_locked();
        }
        if (s_ctx.bmp390_initialized && !s_ctx.bmp390.owns_bus && s_ctx.bmp390.bus_handle == s_ctx.scd41.bus_handle) {
            sensors_deinit_bmp390_locked();
        }
    }
    sensors_deinit_scd41_locked();
    xSemaphoreGive(s_ctx.lock);
}

static void sensors_reset_sgp41(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.sgp41.owns_bus) {
        if (s_ctx.scd41_initialized && !s_ctx.scd41.owns_bus && s_ctx.scd41.bus_handle == s_ctx.sgp41.bus_handle) {
            sensors_deinit_scd41_locked();
        }
        if (s_ctx.bmp390_initialized && !s_ctx.bmp390.owns_bus && s_ctx.bmp390.bus_handle == s_ctx.sgp41.bus_handle) {
            sensors_deinit_bmp390_locked();
        }
    }
    sensors_deinit_sgp41_locked();
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

static uint32_t sensors_remaining_seconds(int64_t started_ms, int64_t duration_ms, int64_t now_ms)
{
    if (duration_ms <= 0) {
        return 0;
    }

    /* started_ms may be negative after restoring a checkpoint: it means
     * the measurement logically started before the current boot.  A value
     * of exactly 0 means "not started yet". */
    int64_t elapsed_ms = (started_ms != 0) ? now_ms - started_ms : 0;
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }
    if (elapsed_ms >= duration_ms) {
        return 0;
    }
    return (uint32_t)((duration_ms - elapsed_ms + 999) / 1000);
}

static void sensors_update_sgp41_validity_locked(int64_t now_ms)
{
    if (!s_ctx.sgp41_initialized || s_ctx.sgp41_measurement_started_ms == 0) {
        s_ctx.snapshot.sgp41_valid = false;
        s_ctx.snapshot.sgp41_conditioning = false;
        s_ctx.snapshot.sgp41_voc_valid = false;
        s_ctx.snapshot.sgp41_nox_valid = false;
        s_ctx.snapshot.sgp41_voc_stabilization_remaining_s = 0;
        s_ctx.snapshot.sgp41_nox_stabilization_remaining_s = 0;
        return;
    }

    s_ctx.snapshot.sgp41_voc_stabilization_remaining_s =
        sensors_remaining_seconds(s_ctx.sgp41_measurement_started_ms, SGP41_VOC_STABILIZATION_MS, now_ms);
    s_ctx.snapshot.sgp41_nox_stabilization_remaining_s =
        sensors_remaining_seconds(s_ctx.sgp41_measurement_started_ms, SGP41_NOX_STABILIZATION_MS, now_ms);

    s_ctx.snapshot.sgp41_conditioning = s_ctx.sgp41_conditioning_remaining_s > 0;
    s_ctx.snapshot.sgp41_voc_valid =
        !s_ctx.snapshot.sgp41_conditioning && s_ctx.snapshot.sgp41_voc_stabilization_remaining_s == 0;
    s_ctx.snapshot.sgp41_nox_valid =
        !s_ctx.snapshot.sgp41_conditioning && s_ctx.snapshot.sgp41_nox_stabilization_remaining_s == 0;
    s_ctx.snapshot.sgp41_valid = s_ctx.snapshot.sgp41_voc_valid || s_ctx.snapshot.sgp41_nox_valid;
}

static void sensors_reset_bmp390(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.bmp390.owns_bus) {
        if (s_ctx.scd41_initialized && !s_ctx.scd41.owns_bus && s_ctx.scd41.bus_handle == s_ctx.bmp390.bus_handle) {
            sensors_deinit_scd41_locked();
        }
        if (s_ctx.sgp41_initialized && !s_ctx.sgp41.owns_bus && s_ctx.sgp41.bus_handle == s_ctx.bmp390.bus_handle) {
            sensors_deinit_sgp41_locked();
        }
    }
    sensors_deinit_bmp390_locked();
    xSemaphoreGive(s_ctx.lock);
}

static bool sensors_i2c_config_matches(int port_a, int sda_a, int scl_a, int port_b, int sda_b, int scl_b)
{
    return port_a == port_b && sda_a == sda_b && scl_a == scl_b;
}

static i2c_master_bus_handle_t sensors_find_shared_i2c_bus_locked(int target_port, int target_sda, int target_scl)
{
    if (s_ctx.scd41_initialized &&
        s_ctx.scd41.bus_handle != NULL &&
        sensors_i2c_config_matches(target_port,
                                   target_sda,
                                   target_scl,
                                   CONFIG_AIRMON_SCD41_I2C_PORT_NUM,
                                   CONFIG_AIRMON_SCD41_I2C_SDA_GPIO,
                                   CONFIG_AIRMON_SCD41_I2C_SCL_GPIO)) {
        return s_ctx.scd41.bus_handle;
    }
    if (s_ctx.sgp41_initialized &&
        s_ctx.sgp41.bus_handle != NULL &&
        sensors_i2c_config_matches(target_port,
                                   target_sda,
                                   target_scl,
                                   CONFIG_AIRMON_SGP41_I2C_PORT_NUM,
                                   CONFIG_AIRMON_SGP41_I2C_SDA_GPIO,
                                   CONFIG_AIRMON_SGP41_I2C_SCL_GPIO)) {
        return s_ctx.sgp41.bus_handle;
    }
    if (s_ctx.bmp390_initialized &&
        s_ctx.bmp390.bus_handle != NULL &&
        sensors_i2c_config_matches(target_port,
                                   target_sda,
                                   target_scl,
                                   CONFIG_AIRMON_BMP390_I2C_PORT_NUM,
                                   CONFIG_AIRMON_BMP390_I2C_SDA_GPIO,
                                   CONFIG_AIRMON_BMP390_I2C_SCL_GPIO)) {
        return s_ctx.bmp390.bus_handle;
    }
    return NULL;
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

static esp_err_t sensors_update_scd41_ambient_pressure_locked(float pressure_hpa, int64_t now_ms)
{
    if (!s_ctx.scd41_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t pressure_pa = (uint32_t)(pressure_hpa * 100.0f + 0.5f);
    if (pressure_pa < 70000U || pressure_pa > 120000U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t applied_pressure_pa = ((pressure_pa + 50U) / 100U) * 100U;
    if (s_ctx.snapshot.co2_compensation_source == CO2_COMPENSATION_SOURCE_BMP390 &&
        s_ctx.co2_compensation_pressure_pa == applied_pressure_pa) {
        s_ctx.co2_compensation_updated_at_ms = now_ms;
        return ESP_OK;
    }

    esp_err_t err = scd41_set_ambient_pressure(&s_ctx.scd41, pressure_pa);
    if (err == ESP_OK) {
        sensors_set_bmp390_co2_compensation_locked(applied_pressure_pa, now_ms);
    }
    return err;
}

static esp_err_t sensors_init_scd41(void)
{
    esp_err_t err = ESP_OK;
    i2c_master_bus_handle_t shared_bus = NULL;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.scd41_initialized) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    memset(&s_ctx.scd41, 0, sizeof(s_ctx.scd41));
    sensors_mark_scd41_invalid_locked();
    shared_bus = sensors_find_shared_i2c_bus_locked(CONFIG_AIRMON_SCD41_I2C_PORT_NUM,
                                                    CONFIG_AIRMON_SCD41_I2C_SDA_GPIO,
                                                    CONFIG_AIRMON_SCD41_I2C_SCL_GPIO);

    if (shared_bus != NULL) {
        err = scd41_init_on_bus(&s_ctx.scd41, shared_bus);
    } else {
        err = scd41_init(&s_ctx.scd41,
                         CONFIG_AIRMON_SCD41_I2C_PORT_NUM,
                         CONFIG_AIRMON_SCD41_I2C_SDA_GPIO,
                         CONFIG_AIRMON_SCD41_I2C_SCL_GPIO);
    }
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
        sensors_set_static_co2_compensation_locked();
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
    sgp41_persisted_state_t persisted = {0};
    esp_err_t err = ESP_OK;
    i2c_master_bus_handle_t shared_bus = NULL;
    bool restored = false;
    int64_t now_ms = 0;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.sgp41_initialized) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    memset(&s_ctx.sgp41, 0, sizeof(s_ctx.sgp41));
    sensors_mark_sgp41_invalid_locked();
    shared_bus = sensors_find_shared_i2c_bus_locked(CONFIG_AIRMON_SGP41_I2C_PORT_NUM,
                                                    CONFIG_AIRMON_SGP41_I2C_SDA_GPIO,
                                                    CONFIG_AIRMON_SGP41_I2C_SCL_GPIO);

    if (shared_bus != NULL) {
        err = sgp41_init_on_bus(&s_ctx.sgp41, shared_bus);
    } else {
        err = sgp41_init(&s_ctx.sgp41,
                         CONFIG_AIRMON_SGP41_I2C_PORT_NUM,
                         CONFIG_AIRMON_SGP41_I2C_SDA_GPIO,
                         CONFIG_AIRMON_SGP41_I2C_SCL_GPIO);
    }
    if (err == ESP_OK) {
        err = sgp41_get_serial_number(&s_ctx.sgp41, serial_number);
    }
    if (err == ESP_OK) {
        GasIndexAlgorithm_init(&s_ctx.sgp41_voc_params, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
        GasIndexAlgorithm_init(&s_ctx.sgp41_nox_params, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);
        s_ctx.sgp41_conditioning_remaining_s = SGP41_CONDITIONING_SEC;
        now_ms = esp_timer_get_time() / 1000;
        s_ctx.sgp41_measurement_started_ms = now_ms;
        if (sensors_load_sgp41_state(&persisted) == ESP_OK &&
            memcmp(persisted.serial_number, serial_number, sizeof(serial_number)) == 0) {
            sensors_deserialize_sgp41_algorithm_state(&persisted.voc_state, &s_ctx.sgp41_voc_params);
            sensors_deserialize_sgp41_algorithm_state(&persisted.nox_state, &s_ctx.sgp41_nox_params);
            s_ctx.sgp41_conditioning_remaining_s =
                persisted.conditioning_remaining_s > SGP41_CONDITIONING_SEC ? SGP41_CONDITIONING_SEC : persisted.conditioning_remaining_s;
            /* Allow negative values: at early boot now_ms is tiny, so
             * subtracting the elapsed seconds intentionally yields a
             * negative timestamp that represents "started before boot".
             * sensors_remaining_seconds() handles this correctly. */
            s_ctx.sgp41_measurement_started_ms =
                now_ms - ((int64_t)persisted.measurement_elapsed_s * 1000LL);
            if (s_ctx.sgp41_measurement_started_ms > now_ms) {
                /* Overflow / corrupt data guard – fall back to fresh start */
                s_ctx.sgp41_measurement_started_ms = now_ms;
            }
            s_ctx.snapshot.voc_index = persisted.voc_index;
            s_ctx.snapshot.nox_index = persisted.nox_index;
            restored = true;
            ESP_LOGI(TAG,
                     "SGP41 checkpoint restored: elapsed=%" PRIu32 "s conditioning=%u voc=%" PRId32 " nox=%" PRId32,
                     persisted.measurement_elapsed_s,
                     persisted.conditioning_remaining_s,
                     persisted.voc_index,
                     persisted.nox_index);
        } else {
            ESP_LOGI(TAG, "SGP41 checkpoint unavailable or serial mismatch; starting fresh learning");
        }
        s_ctx.sgp41_initialized = true;
        s_ctx.sgp41_state_saved_at_ms = now_ms;
        s_ctx.sgp41_state_dirty = !restored;
        sensors_update_sgp41_validity_locked(now_ms);
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

static esp_err_t sensors_init_bmp390(void)
{
    esp_err_t err = ESP_OK;
    i2c_master_bus_handle_t shared_bus = NULL;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (s_ctx.bmp390_initialized) {
        xSemaphoreGive(s_ctx.lock);
        return ESP_OK;
    }

    memset(&s_ctx.bmp390, 0, sizeof(s_ctx.bmp390));
    sensors_mark_bmp390_invalid_locked();
    shared_bus = sensors_find_shared_i2c_bus_locked(CONFIG_AIRMON_BMP390_I2C_PORT_NUM,
                                                    CONFIG_AIRMON_BMP390_I2C_SDA_GPIO,
                                                    CONFIG_AIRMON_BMP390_I2C_SCL_GPIO);

    if (shared_bus != NULL) {
        err = bmp390_init_on_bus(&s_ctx.bmp390, shared_bus, CONFIG_AIRMON_BMP390_I2C_ADDRESS);
    } else {
        err = bmp390_init(&s_ctx.bmp390,
                          CONFIG_AIRMON_BMP390_I2C_PORT_NUM,
                          CONFIG_AIRMON_BMP390_I2C_SDA_GPIO,
                          CONFIG_AIRMON_BMP390_I2C_SCL_GPIO,
                          CONFIG_AIRMON_BMP390_I2C_ADDRESS);
    }

    if (err == ESP_OK) {
        s_ctx.bmp390_initialized = true;
        /* Try to restore pressure ring buffer from NVS */
        int64_t init_now_ms = esp_timer_get_time() / 1000;
        if (sensors_load_bmp390_history(init_now_ms) != ESP_OK) {
            ESP_LOGI(TAG, "BMP390 pressure history: starting fresh");
        }
    } else if (s_ctx.bmp390.dev_handle != NULL || s_ctx.bmp390.bus_handle != NULL) {
        bmp390_deinit(&s_ctx.bmp390);
        memset(&s_ctx.bmp390, 0, sizeof(s_ctx.bmp390));
    }
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        sensors_clear_bmp390_error();
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
        uint32_t auto_cleaning_interval_sec = 0;
        err = sps30_read_auto_cleaning_interval(&s_ctx.sps30, &auto_cleaning_interval_sec);
        if (err == ESP_OK && auto_cleaning_interval_sec != CONFIG_AIRMON_SPS30_AUTO_CLEANING_INTERVAL_SEC) {
            err = sps30_set_auto_cleaning_interval(&s_ctx.sps30, CONFIG_AIRMON_SPS30_AUTO_CLEANING_INTERVAL_SEC);
        }
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

static void sensors_apply_bmp390_measurement(float temperature_c, float pressure_hpa, int64_t now_ms)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.snapshot.bmp390_valid = true;
    s_ctx.snapshot.bmp390_temperature_c = temperature_c;
    s_ctx.snapshot.pressure_hpa = pressure_hpa;
    sensors_update_bmp390_pressure_trend_locked(pressure_hpa, now_ms);
    s_ctx.snapshot.updated_at_ms = now_ms;
    xSemaphoreGive(s_ctx.lock);
}

static void sensor_task(void *arg)
{
    int64_t last_scd_check_ms = 0;
    int64_t last_sgp41_check_ms = 0;
    int64_t last_bmp390_check_ms = 0;
    int64_t last_reinit_ms = 0;

    while (s_ctx.running) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (!s_ctx.scd41_initialized || !s_ctx.sgp41_initialized || !s_ctx.bmp390_initialized || !s_ctx.sps30_initialized) {
            if (now_ms - last_reinit_ms > 30000) {
                if (!s_ctx.scd41_initialized && sensors_init_scd41() != ESP_OK) {
                    sensors_mark_scd41_invalid();
                    sensors_set_scd41_error("init failed");
                }
                if (!s_ctx.sgp41_initialized && sensors_init_sgp41() != ESP_OK) {
                    sensors_mark_sgp41_invalid();
                    sensors_set_sgp41_error("init failed");
                }
                if (!s_ctx.bmp390_initialized && sensors_init_bmp390() != ESP_OK) {
                    sensors_mark_bmp390_invalid();
                    sensors_set_bmp390_error("init failed");
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
            } else if (err == ESP_ERR_NOT_FINISHED) {
                /* Read may legally return an empty response if the next sample is not ready yet. */
            } else if (err != ESP_OK) {
                sensors_reset_sps30();
                sensors_set_sps30_error(ready ? "read failed" : "ready check failed");
            }
        }

        if (s_ctx.bmp390_initialized && now_ms - last_bmp390_check_ms >= 1000) {
            bool ready = false;
            bool measurement_ok = false;
            float temperature = 0.0f;
            float pressure = 0.0f;
            esp_err_t err = ESP_OK;

            last_bmp390_check_ms = now_ms;
            xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
            if (s_ctx.bmp390_initialized) {
                err = bmp390_data_ready(&s_ctx.bmp390, &ready);
                if (err == ESP_OK && ready) {
                    err = bmp390_read_measurement(&s_ctx.bmp390, &temperature, &pressure);
                    measurement_ok = err == ESP_OK;
                }
            }
            xSemaphoreGive(s_ctx.lock);

            if (err == ESP_OK && ready && measurement_ok) {
                bool scd41_ready = false;
                esp_err_t compensation_err = ESP_OK;

                xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
                scd41_ready = s_ctx.scd41_initialized;
                if (scd41_ready) {
                    compensation_err = sensors_update_scd41_ambient_pressure_locked(pressure, now_ms);
                }
                xSemaphoreGive(s_ctx.lock);

                sensors_apply_bmp390_measurement(temperature, pressure, now_ms);
                sensors_clear_bmp390_error();
                if (scd41_ready && compensation_err != ESP_OK) {
                    sensors_set_scd41_error(compensation_err == ESP_ERR_INVALID_ARG
                                                ? "ambient pressure out of range"
                                                : "ambient pressure update failed");
                }
            } else if (err != ESP_OK) {
                sensors_reset_bmp390();
                sensors_set_bmp390_error(ready ? "read failed" : "ready check failed");
            }
        }

        uint16_t fallback_altitude_m = 0;
        float fallback_temp_offset_c = 0.0f;
        bool restore_static_compensation = false;

        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        if (s_ctx.scd41_initialized &&
            s_ctx.snapshot.co2_compensation_source == CO2_COMPENSATION_SOURCE_BMP390 &&
            s_ctx.co2_compensation_updated_at_ms > 0 &&
            (now_ms - s_ctx.co2_compensation_updated_at_ms) >= SCD41_PRESSURE_COMPENSATION_FALLBACK_MS) {
            fallback_altitude_m = s_ctx.config.scd41_altitude_m;
            fallback_temp_offset_c = s_ctx.config.scd41_temp_offset_c;
            restore_static_compensation = true;
        }
        xSemaphoreGive(s_ctx.lock);

        if (restore_static_compensation) {
            if (sensors_set_scd41_compensation(fallback_altitude_m, fallback_temp_offset_c) != ESP_OK) {
                sensors_reset_scd41();
                sensors_set_scd41_error("fallback compensation restore failed");
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
                        if (s_ctx.sgp41_conditioning_remaining_s > 0) {
                            s_ctx.sgp41_conditioning_remaining_s--;
                        }
                        sensors_update_sgp41_validity_locked(now_ms);
                        s_ctx.sgp41_state_dirty = true;
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

                        s_ctx.snapshot.voc_index = voc_index;
                        s_ctx.snapshot.nox_index = nox_index;
                        sensors_update_sgp41_validity_locked(now_ms);
                        s_ctx.snapshot.updated_at_ms = now_ms;
                        s_ctx.sgp41_state_dirty = true;
                        sample_ok = true;
                    }
                }
            }
            xSemaphoreGive(s_ctx.lock);

            if (sample_ok) {
                if (!conditioning) {
                    esp_err_t checkpoint_err = sensors_checkpoint_sgp41_state_internal(false);
                    if (checkpoint_err != ESP_OK) {
                        ESP_LOGW(TAG, "failed to checkpoint SGP41 state: %d", checkpoint_err);
                    }
                }
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
                        sensors_update_scd41_humidity_trend_locked(humidity, now_ms);
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

        /* Periodic trend history checkpoints */
        sensors_checkpoint_bmp390_history(false);
        sensors_checkpoint_scd41_humidity_history(false);
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
    } else {
        /* Try to restore humidity history from NVS (SCD41 must be initialized first) */
        int64_t hum_now_ms = esp_timer_get_time() / 1000;
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        if (sensors_load_scd41_humidity_history(hum_now_ms) != ESP_OK) {
            ESP_LOGI(TAG, "SCD41 humidity history: starting fresh");
        }
        xSemaphoreGive(s_ctx.lock);
    }
    if (sensors_init_sgp41() != ESP_OK) {
        sensors_mark_sgp41_invalid();
        sensors_set_sgp41_error("init failed");
    }
    if (sensors_init_bmp390() != ESP_OK) {
        sensors_mark_bmp390_invalid();
        sensors_set_bmp390_error("init failed");
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

esp_err_t sensors_checkpoint_sgp41_state(void)
{
    return sensors_checkpoint_sgp41_state_internal(true);
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
    esp_err_t checkpoint_err = sensors_checkpoint_sgp41_state_internal(true);
    if (checkpoint_err != ESP_OK && checkpoint_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "failed to checkpoint SGP41 state during stop: %d", checkpoint_err);
    }
    sensors_checkpoint_bmp390_history(true);
    sensors_checkpoint_scd41_humidity_history(true);
    if (s_ctx.bmp390_initialized) {
        bmp390_deinit(&s_ctx.bmp390);
        s_ctx.bmp390_initialized = false;
    }
    if (s_ctx.sgp41_initialized) {
        sgp41_turn_heater_off(&s_ctx.sgp41);
        sgp41_deinit(&s_ctx.sgp41);
        s_ctx.sgp41_initialized = false;
    }
    if (s_ctx.scd41_initialized) {
        scd41_stop_periodic_measurement(&s_ctx.scd41);
        scd41_deinit(&s_ctx.scd41);
        s_ctx.scd41_initialized = false;
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
    return s_ctx.scd41_initialized || s_ctx.sgp41_initialized || s_ctx.bmp390_initialized || s_ctx.sps30_initialized;
}

bool sensors_all_ready(void)
{
    return s_ctx.scd41_initialized && s_ctx.sgp41_initialized && s_ctx.bmp390_initialized && s_ctx.sps30_initialized;
}

bool sensors_is_scd41_ready(void)
{
    return s_ctx.scd41_initialized;
}

bool sensors_is_sgp41_ready(void)
{
    return s_ctx.sgp41_initialized;
}

bool sensors_is_bmp390_ready(void)
{
    return s_ctx.bmp390_initialized;
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
        sensors_set_static_co2_compensation_locked();
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
