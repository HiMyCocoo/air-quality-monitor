#include "air_quality_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool air_quality_get_local_time(struct tm *timeinfo_out)
{
    if (timeinfo_out == NULL) {
        return false;
    }

    time_t now = 0;
    time(&now);
    if ((int64_t)now < RAIN_TIME_VALID_AFTER_EPOCH) {
        return false;
    }

    struct tm local_time = {0};
    if (localtime_r(&now, &local_time) == NULL) {
        return false;
    }

    *timeinfo_out = local_time;
    return true;
}

static air_quality_rain_season_t air_quality_detect_hangzhou_rain_season(void)
{
    struct tm local_time = {0};
    if (!air_quality_get_local_time(&local_time)) {
        return AIR_QUALITY_RAIN_SEASON_UNAVAILABLE;
    }

    /* Hangzhou uses a broad Lower-Yangtze climatology window: Jun-Jul are treated as meiyu season. */
    int month = local_time.tm_mon + 1;
    if (month >= 3 && month <= 5) {
        return AIR_QUALITY_RAIN_SEASON_SPRING;
    }
    if (month >= 6 && month <= 7) {
        return AIR_QUALITY_RAIN_SEASON_MEIYU;
    }
    if (month >= 8 && month <= 9) {
        return AIR_QUALITY_RAIN_SEASON_SUMMER;
    }
    if (month >= 10 && month <= 11) {
        return AIR_QUALITY_RAIN_SEASON_AUTUMN;
    }
    return AIR_QUALITY_RAIN_SEASON_WINTER;
}

static bool air_quality_compute_dew_point_spread(float temperature_c, float humidity_rh, float *spread_out)
{
    if (spread_out == NULL ||
        !isfinite(temperature_c) ||
        !isfinite(humidity_rh) ||
        humidity_rh <= 0.0f ||
        humidity_rh > 100.0f) {
        return false;
    }

    const double gamma = log(humidity_rh / 100.0) + (17.62 * temperature_c) / (243.12 + temperature_c);
    const double dew_point_c = (243.12 * gamma) / (17.62 - gamma);
    if (!isfinite(dew_point_c)) {
        return false;
    }

    *spread_out = (float)(temperature_c - dew_point_c);
    return isfinite(*spread_out) && *spread_out > -1.0f && *spread_out < 60.0f;
}

static float air_quality_rain_pressure_score(float pressure_hpa, float pressure_trend_hpa_3h)
{
    float score = 0.0f;

    if (pressure_trend_hpa_3h <= -5.0f) {
        score += 2.7f;
    } else if (pressure_trend_hpa_3h <= -3.5f) {
        score += 1.9f;
    } else if (pressure_trend_hpa_3h <= -2.0f) {
        score += 1.2f;
    } else if (pressure_trend_hpa_3h <= -0.8f) {
        score += 0.5f;
    } else if (pressure_trend_hpa_3h >= 4.0f) {
        score -= 1.2f;
    } else if (pressure_trend_hpa_3h >= 2.0f) {
        score -= 0.7f;
    }

    if (pressure_hpa <= 1004.0f) {
        score += 1.0f;
    } else if (pressure_hpa <= 1008.0f) {
        score += 0.5f;
    } else if (pressure_hpa >= 1018.0f) {
        score -= 0.4f;
    }

    return score;
}

static float air_quality_rain_humidity_score(float humidity_rh,
                                             bool humidity_valid,
                                             float humidity_trend_rh_3h,
                                             bool humidity_trend_valid,
                                             float dew_point_spread_c,
                                             bool dew_point_spread_valid)
{
    float score = 0.0f;

    if (humidity_valid) {
        if (humidity_rh >= 90.0f) {
            score += 0.8f;
        } else if (humidity_rh >= 82.0f) {
            score += 0.4f;
        } else if (humidity_rh <= 55.0f) {
            score -= 0.7f;
        }
    }

    if (humidity_trend_valid) {
        if (humidity_trend_rh_3h >= 12.0f) {
            score += 1.0f;
        } else if (humidity_trend_rh_3h >= 6.0f) {
            score += 0.6f;
        } else if (humidity_trend_rh_3h >= 3.0f) {
            score += 0.3f;
        } else if (humidity_trend_rh_3h <= -8.0f) {
            score -= 0.7f;
        } else if (humidity_trend_rh_3h <= -4.0f) {
            score -= 0.3f;
        }
    }

    if (dew_point_spread_valid) {
        if (dew_point_spread_c <= 1.5f) {
            score += 1.2f;
        } else if (dew_point_spread_c <= 3.0f) {
            score += 0.7f;
        } else if (dew_point_spread_c <= 5.0f) {
            score += 0.3f;
        } else if (dew_point_spread_c >= 8.0f) {
            score -= 0.4f;
        }
    }

    return score;
}

static float air_quality_rain_season_adjustment(air_quality_rain_season_t season,
                                                float pressure_trend_hpa_3h,
                                                bool humidity_valid,
                                                float humidity_rh,
                                                bool humidity_trend_valid,
                                                float humidity_trend_rh_3h)
{
    switch (season) {
    case AIR_QUALITY_RAIN_SEASON_SPRING:
        if (pressure_trend_hpa_3h <= -1.5f &&
            ((humidity_valid && humidity_rh >= 70.0f) ||
             (humidity_trend_valid && humidity_trend_rh_3h >= 4.0f))) {
            return 0.4f;
        }
        return 0.1f;
    case AIR_QUALITY_RAIN_SEASON_MEIYU: {
        float adjustment = 0.6f;
        if (humidity_valid && humidity_rh >= 80.0f) {
            adjustment += 0.4f;
        }
        if (humidity_trend_valid && humidity_trend_rh_3h >= 4.0f) {
            adjustment += 0.2f;
        }
        return adjustment;
    }
    case AIR_QUALITY_RAIN_SEASON_SUMMER:
        if (pressure_trend_hpa_3h <= -2.0f && humidity_valid && humidity_rh >= 75.0f) {
            return 0.5f;
        }
        return -0.1f;
    case AIR_QUALITY_RAIN_SEASON_AUTUMN:
        return -0.4f;
    case AIR_QUALITY_RAIN_SEASON_WINTER:
        return -0.6f;
    case AIR_QUALITY_RAIN_SEASON_UNAVAILABLE:
    default:
        return 0.0f;
    }
}

static air_quality_rain_outlook_t air_quality_rain_outlook_from_score(float score)
{
    if (score >= RAIN_SCORE_LIKELY_THRESHOLD) {
        return AIR_QUALITY_RAIN_OUTLOOK_LIKELY;
    }
    if (score >= RAIN_SCORE_POSSIBLE_THRESHOLD) {
        return AIR_QUALITY_RAIN_OUTLOOK_POSSIBLE;
    }
    if (score >= RAIN_SCORE_SLIGHT_THRESHOLD) {
        return AIR_QUALITY_RAIN_OUTLOOK_SLIGHT_CHANCE;
    }
    return AIR_QUALITY_RAIN_OUTLOOK_UNLIKELY;
}

air_quality_pressure_trend_t air_quality_rate_pressure_trend(float pressure_trend_hpa_3h, bool valid)
{
    if (!valid || !isfinite(pressure_trend_hpa_3h)) {
        return AIR_QUALITY_PRESSURE_TREND_UNAVAILABLE;
    }
    if (pressure_trend_hpa_3h <= PRESSURE_TREND_FALLING_FAST_THRESHOLD_HPA_3H) {
        return AIR_QUALITY_PRESSURE_TREND_FALLING_FAST;
    }
    if (pressure_trend_hpa_3h <= PRESSURE_TREND_FALLING_THRESHOLD_HPA_3H) {
        return AIR_QUALITY_PRESSURE_TREND_FALLING;
    }
    if (pressure_trend_hpa_3h < PRESSURE_TREND_RISING_THRESHOLD_HPA_3H) {
        return AIR_QUALITY_PRESSURE_TREND_STABLE;
    }
    if (pressure_trend_hpa_3h < PRESSURE_TREND_RISING_FAST_THRESHOLD_HPA_3H) {
        return AIR_QUALITY_PRESSURE_TREND_RISING;
    }
    return AIR_QUALITY_PRESSURE_TREND_RISING_FAST;
}

air_quality_rain_outlook_t air_quality_estimate_rain_outlook(float pressure_hpa,
                                                             float pressure_trend_hpa_3h,
                                                             bool valid)
{
    if (!valid || !isfinite(pressure_hpa) || !isfinite(pressure_trend_hpa_3h) || pressure_hpa <= 0.0f) {
        return AIR_QUALITY_RAIN_OUTLOOK_UNAVAILABLE;
    }
    if (pressure_trend_hpa_3h <= RAIN_OUTLOOK_LIKELY_THRESHOLD_HPA_3H ||
        (pressure_hpa <= RAIN_OUTLOOK_VERY_LOW_PRESSURE_HPA &&
         pressure_trend_hpa_3h <= -3.0f)) {
        return AIR_QUALITY_RAIN_OUTLOOK_LIKELY;
    }
    if (pressure_trend_hpa_3h <= RAIN_OUTLOOK_POSSIBLE_THRESHOLD_HPA_3H ||
        (pressure_hpa <= RAIN_OUTLOOK_LOW_PRESSURE_HPA &&
         pressure_trend_hpa_3h <= -2.0f)) {
        return AIR_QUALITY_RAIN_OUTLOOK_POSSIBLE;
    }
    if (pressure_trend_hpa_3h <= RAIN_OUTLOOK_SLIGHT_THRESHOLD_HPA_3H) {
        return AIR_QUALITY_RAIN_OUTLOOK_SLIGHT_CHANCE;
    }
    return AIR_QUALITY_RAIN_OUTLOOK_UNLIKELY;
}

void air_quality_analyze_rain(const sensor_snapshot_t *snapshot, air_quality_rain_analysis_t *result)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->pressure_trend = AIR_QUALITY_PRESSURE_TREND_UNAVAILABLE;
    result->outlook = AIR_QUALITY_RAIN_OUTLOOK_UNAVAILABLE;
    result->season = air_quality_detect_hangzhou_rain_season();

    if (snapshot == NULL) {
        strlcpy(result->basis, "No sensor data", sizeof(result->basis));
        return;
    }

    const bool pressure_valid =
        snapshot->bmp390_valid &&
        snapshot->pressure_trend_valid &&
        isfinite(snapshot->pressure_hpa) &&
        snapshot->pressure_hpa > 0.0f &&
        isfinite(snapshot->pressure_trend_hpa_3h);
    const bool humidity_valid =
        snapshot->scd41_valid &&
        isfinite(snapshot->humidity_rh) &&
        snapshot->humidity_rh >= 0.0f &&
        snapshot->humidity_rh <= 100.0f;
    const bool humidity_trend_valid =
        snapshot->humidity_trend_valid &&
        isfinite(snapshot->humidity_trend_rh_3h);

    result->pressure_trend =
        air_quality_rate_pressure_trend(snapshot->pressure_trend_hpa_3h, pressure_valid);
    result->dew_point_spread_valid =
        humidity_valid &&
        air_quality_compute_dew_point_spread(snapshot->temperature_c,
                                             snapshot->humidity_rh,
                                             &result->dew_point_spread_c);

    if (!pressure_valid) {
        strlcpy(result->basis, "Need about 1 hour of continuous BMP390 pressure history", sizeof(result->basis));
        return;
    }

    float score = air_quality_rain_pressure_score(snapshot->pressure_hpa, snapshot->pressure_trend_hpa_3h);
    score += air_quality_rain_humidity_score(snapshot->humidity_rh,
                                             humidity_valid,
                                             snapshot->humidity_trend_rh_3h,
                                             humidity_trend_valid,
                                             result->dew_point_spread_c,
                                             result->dew_point_spread_valid);
    score += air_quality_rain_season_adjustment(result->season,
                                                snapshot->pressure_trend_hpa_3h,
                                                humidity_valid,
                                                snapshot->humidity_rh,
                                                humidity_trend_valid,
                                                snapshot->humidity_trend_rh_3h);

    result->valid = true;
    result->outlook = air_quality_rain_outlook_from_score(score);

    if (result->season == AIR_QUALITY_RAIN_SEASON_UNAVAILABLE) {
        strlcpy(result->basis, "Hangzhou heuristic, neutral season until clock sync;", sizeof(result->basis));
    } else {
        snprintf(result->basis,
                 sizeof(result->basis),
                 "Hangzhou %s heuristic;",
                 air_quality_rain_season_label(result->season));
    }

    char fragment[48];
    snprintf(fragment, sizeof(fragment), "pressure %+0.1f hPa/3h.", snapshot->pressure_trend_hpa_3h);
    air_quality_append_note(result->basis, sizeof(result->basis), fragment);

    if (humidity_trend_valid) {
        snprintf(fragment, sizeof(fragment), "RH %+0.1f%%/3h.", snapshot->humidity_trend_rh_3h);
        air_quality_append_note(result->basis, sizeof(result->basis), fragment);
    } else if (humidity_valid) {
        snprintf(fragment, sizeof(fragment), "RH %.0f%% now.", snapshot->humidity_rh);
        air_quality_append_note(result->basis, sizeof(result->basis), fragment);
    } else {
        air_quality_append_note(result->basis, sizeof(result->basis), "Humidity trend unavailable.");
    }

    if (result->dew_point_spread_valid) {
        snprintf(fragment, sizeof(fragment), "Dew-point spread %.1fC.", result->dew_point_spread_c);
        air_quality_append_note(result->basis, sizeof(result->basis), fragment);
    }
}
