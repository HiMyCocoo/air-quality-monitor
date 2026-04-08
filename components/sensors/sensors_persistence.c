#include "sensors_internal.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "sensors";

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

void sensors_deserialize_sgp41_algorithm_state(const sgp41_persisted_algorithm_state_t *state,
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

esp_err_t sensors_load_sgp41_state(sgp41_persisted_state_t *state)
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

esp_err_t sensors_checkpoint_sgp41_state_internal(bool force)
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

typedef struct {
    int64_t offset_ms;
    float pressure_hpa;
} bmp390_persisted_entry_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t write_index;
    bmp390_persisted_entry_t entries[BMP390_PRESSURE_HISTORY_LEN];
} bmp390_persisted_history_t;

static void sensors_serialize_bmp390_history_locked(bmp390_persisted_history_t *persisted, int64_t now_ms)
{
    memset(persisted, 0, sizeof(*persisted));
    persisted->version = TREND_STATE_VERSION;
    persisted->count = (uint32_t)s_ctx.bmp390_pressure_history_count;
    persisted->write_index = (uint32_t)s_ctx.bmp390_pressure_history_index;

    for (size_t i = 0; i < s_ctx.bmp390_pressure_history_count; ++i) {
        persisted->entries[i].pressure_hpa = s_ctx.bmp390_pressure_history[i].pressure_hpa;
        persisted->entries[i].offset_ms = now_ms - s_ctx.bmp390_pressure_history[i].captured_at_ms;
    }
}

static esp_err_t sensors_store_bmp390_history(const bmp390_persisted_history_t *persisted)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TREND_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, BMP390_HISTORY_KEY, persisted, sizeof(*persisted));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t sensors_load_bmp390_history(int64_t now_ms)
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

void sensors_checkpoint_bmp390_history(bool force)
{
    if (s_ctx.lock == NULL) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    bmp390_persisted_history_t persisted = {0};
    uint32_t saved_count = 0;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    bool should_save = s_ctx.bmp390_history_dirty &&
                       (force || (now_ms - s_ctx.bmp390_history_saved_at_ms) >= TREND_STATE_SAVE_INTERVAL_MS);
    if (!should_save || s_ctx.bmp390_pressure_history_count == 0) {
        xSemaphoreGive(s_ctx.lock);
        return;
    }
    sensors_serialize_bmp390_history_locked(&persisted, now_ms);
    saved_count = (uint32_t)s_ctx.bmp390_pressure_history_count;
    xSemaphoreGive(s_ctx.lock);

    esp_err_t err = sensors_store_bmp390_history(&persisted);
    if (err == ESP_OK) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        s_ctx.bmp390_history_dirty = false;
        s_ctx.bmp390_history_saved_at_ms = now_ms;
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGI(TAG, "BMP390 pressure history checkpoint saved: %u samples", (unsigned)saved_count);
    }
}

typedef struct {
    int64_t offset_ms;
    float humidity_rh;
} scd41_persisted_humidity_entry_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t write_index;
    scd41_persisted_humidity_entry_t entries[SCD41_HUMIDITY_HISTORY_LEN];
} scd41_persisted_humidity_history_t;

static void sensors_serialize_scd41_humidity_history_locked(scd41_persisted_humidity_history_t *persisted, int64_t now_ms)
{
    memset(persisted, 0, sizeof(*persisted));
    persisted->version = TREND_STATE_VERSION;
    persisted->count = (uint32_t)s_ctx.scd41_humidity_history_count;
    persisted->write_index = (uint32_t)s_ctx.scd41_humidity_history_index;

    for (size_t i = 0; i < s_ctx.scd41_humidity_history_count; ++i) {
        persisted->entries[i].humidity_rh = s_ctx.scd41_humidity_history[i].humidity_rh;
        persisted->entries[i].offset_ms = now_ms - s_ctx.scd41_humidity_history[i].captured_at_ms;
    }
}

static esp_err_t sensors_store_scd41_humidity_history(const scd41_persisted_humidity_history_t *persisted)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TREND_STATE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, SCD41_HUMIDITY_HISTORY_KEY, persisted, sizeof(*persisted));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t sensors_load_scd41_humidity_history(int64_t now_ms)
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
        s_ctx.scd41_humidity_history[i].humidity_rh = persisted.entries[i].humidity_rh;
        s_ctx.scd41_humidity_history[i].captured_at_ms = now_ms - persisted.entries[i].offset_ms;
    }

    ESP_LOGI(TAG, "SCD41 humidity history restored: %u samples", (unsigned)persisted.count);
    return ESP_OK;
}

void sensors_checkpoint_scd41_humidity_history(bool force)
{
    if (s_ctx.lock == NULL) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    scd41_persisted_humidity_history_t persisted = {0};
    uint32_t saved_count = 0;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    bool should_save = s_ctx.scd41_humidity_history_dirty &&
                       (force || (now_ms - s_ctx.scd41_humidity_history_saved_at_ms) >= TREND_STATE_SAVE_INTERVAL_MS);
    if (!should_save || s_ctx.scd41_humidity_history_count == 0) {
        xSemaphoreGive(s_ctx.lock);
        return;
    }
    sensors_serialize_scd41_humidity_history_locked(&persisted, now_ms);
    saved_count = (uint32_t)s_ctx.scd41_humidity_history_count;
    xSemaphoreGive(s_ctx.lock);

    esp_err_t err = sensors_store_scd41_humidity_history(&persisted);
    if (err == ESP_OK) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        s_ctx.scd41_humidity_history_dirty = false;
        s_ctx.scd41_humidity_history_saved_at_ms = now_ms;
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGI(TAG, "SCD41 humidity history checkpoint saved: %u samples", (unsigned)saved_count);
    }
}
