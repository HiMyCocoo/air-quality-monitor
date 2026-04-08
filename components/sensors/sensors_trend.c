#include "sensors_internal.h"

#include <math.h>

void sensors_clear_bmp390_pressure_history_locked(void)
{
    s_ctx.bmp390_pressure_history_count = 0;
    s_ctx.bmp390_pressure_history_index = 0;
}

void sensors_seed_bmp390_pressure_history_locked(float pressure_hpa, int64_t now_ms)
{
    s_ctx.bmp390_pressure_history[0].pressure_hpa = pressure_hpa;
    s_ctx.bmp390_pressure_history[0].captured_at_ms = now_ms;
    s_ctx.bmp390_pressure_history_count = 1;
    s_ctx.bmp390_pressure_history_index = 1 % BMP390_PRESSURE_HISTORY_LEN;
    s_ctx.bmp390_history_dirty = true;
}

void sensors_clear_scd41_humidity_history_locked(void)
{
    s_ctx.scd41_humidity_history_count = 0;
    s_ctx.scd41_humidity_history_index = 0;
}

void sensors_seed_scd41_humidity_history_locked(float humidity_rh, int64_t now_ms)
{
    s_ctx.scd41_humidity_history[0].humidity_rh = humidity_rh;
    s_ctx.scd41_humidity_history[0].captured_at_ms = now_ms;
    s_ctx.scd41_humidity_history_count = 1;
    s_ctx.scd41_humidity_history_index = 1 % SCD41_HUMIDITY_HISTORY_LEN;
    s_ctx.scd41_humidity_history_dirty = true;
}

void sensors_update_bmp390_pressure_trend_locked(float pressure_hpa, int64_t now_ms)
{
    s_ctx.snapshot.pressure_trend_valid = false;
    s_ctx.snapshot.pressure_trend_hpa_3h = 0.0f;
    s_ctx.snapshot.pressure_trend_span_min = 0;

    if (!isfinite(pressure_hpa) || pressure_hpa <= 0.0f) {
        return;
    }

    if (s_ctx.bmp390_pressure_history_count == 0) {
        sensors_seed_bmp390_pressure_history_locked(pressure_hpa, now_ms);
        return;
    }

    size_t newest_index =
        (s_ctx.bmp390_pressure_history_index + BMP390_PRESSURE_HISTORY_LEN - 1) % BMP390_PRESSURE_HISTORY_LEN;
    int64_t newest_at_ms = s_ctx.bmp390_pressure_history[newest_index].captured_at_ms;
    int64_t gap_ms = now_ms - newest_at_ms;
    if (gap_ms >= BMP390_PRESSURE_HISTORY_MAX_GAP_MS) {
        sensors_clear_bmp390_pressure_history_locked();
        sensors_seed_bmp390_pressure_history_locked(pressure_hpa, now_ms);
        return;
    }

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
        if (!isfinite(candidate_pressure_hpa) || candidate_pressure_hpa <= 0.0f) {
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

void sensors_update_scd41_humidity_trend_locked(float humidity_rh, int64_t now_ms)
{
    s_ctx.snapshot.humidity_trend_valid = false;
    s_ctx.snapshot.humidity_trend_rh_3h = 0.0f;
    s_ctx.snapshot.humidity_trend_span_min = 0;

    if (!isfinite(humidity_rh) || humidity_rh < 0.0f || humidity_rh > 100.0f) {
        return;
    }

    if (s_ctx.scd41_humidity_history_count == 0) {
        sensors_seed_scd41_humidity_history_locked(humidity_rh, now_ms);
        return;
    }

    size_t newest_index =
        (s_ctx.scd41_humidity_history_index + SCD41_HUMIDITY_HISTORY_LEN - 1) % SCD41_HUMIDITY_HISTORY_LEN;
    int64_t newest_at_ms = s_ctx.scd41_humidity_history[newest_index].captured_at_ms;
    int64_t gap_ms = now_ms - newest_at_ms;
    if (gap_ms >= SCD41_HUMIDITY_HISTORY_MAX_GAP_MS) {
        sensors_clear_scd41_humidity_history_locked();
        sensors_seed_scd41_humidity_history_locked(humidity_rh, now_ms);
        return;
    }

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
        if (!isfinite(candidate_humidity_rh) || candidate_humidity_rh < 0.0f || candidate_humidity_rh > 100.0f) {
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
