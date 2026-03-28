#include "air_quality.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    double concentration_low;
    double concentration_high;
    int index_low;
    int index_high;
} aqi_breakpoint_t;

#define CO2_GOOD_MAX_PPM 800U
#define CO2_ACCEPTABLE_MAX_PPM 1000U
#define CO2_ELEVATED_MAX_PPM 1200U
#define CO2_HIGH_MAX_PPM 2000U
#define CO2_VENTILATION_PROXY_ALERT_PPM CO2_ACCEPTABLE_MAX_PPM
#define HUMIDITY_RECOMMENDED_MIN_RH 30.0
#define HUMIDITY_RECOMMENDED_MAX_RH 60.0
#define TEMPERATURE_RECOMMENDED_MIN_C 20.0
#define TEMPERATURE_RECOMMENDED_MAX_C 26.0
#define VOC_GOOD_MAX_INDEX 100
#define VOC_ACCEPTABLE_MAX_INDEX 150
#define VOC_ELEVATED_MAX_INDEX 230
#define VOC_HIGH_MAX_INDEX 350
#define NOX_GOOD_MAX_INDEX 2
#define NOX_ACCEPTABLE_MAX_INDEX 20
#define NOX_ELEVATED_MAX_INDEX 50
#define NOX_HIGH_MAX_INDEX 100
#define PARTICLE_FINE_RATIO_HIGH 0.60
#define PARTICLE_FINE_RATIO_VERY_HIGH 0.70
#define PARTICLE_COARSE_RATIO_HIGH 0.50
#define PARTICLE_FINE_TO_PM10_DUST_LIKE_MAX 0.35
#define PARTICLE_TYPICAL_FINE_MAX_UM 1.0f
#define PARTICLE_TYPICAL_COARSE_MIN_UM 2.5f
#define PRESSURE_TREND_RISING_THRESHOLD_HPA_3H 1.5f
#define PRESSURE_TREND_RISING_FAST_THRESHOLD_HPA_3H 4.0f
#define PRESSURE_TREND_FALLING_THRESHOLD_HPA_3H -1.5f
#define PRESSURE_TREND_FALLING_FAST_THRESHOLD_HPA_3H -4.0f
#define RAIN_OUTLOOK_SLIGHT_THRESHOLD_HPA_3H -1.0f
#define RAIN_OUTLOOK_POSSIBLE_THRESHOLD_HPA_3H -2.5f
#define RAIN_OUTLOOK_LIKELY_THRESHOLD_HPA_3H -4.5f
#define RAIN_OUTLOOK_LOW_PRESSURE_HPA 1009.0f
#define RAIN_OUTLOOK_VERY_LOW_PRESSURE_HPA 1005.0f
#define RAIN_SCORE_SLIGHT_THRESHOLD 0.8f
#define RAIN_SCORE_POSSIBLE_THRESHOLD 2.0f
#define RAIN_SCORE_LIKELY_THRESHOLD 3.5f
#define RAIN_TIME_VALID_AFTER_EPOCH 1704067200LL

static const aqi_breakpoint_t PM2_5_BREAKPOINTS[] = {
    {0.0, 9.0, 0, 50},
    {9.1, 35.4, 51, 100},
    {35.5, 55.4, 101, 150},
    {55.5, 125.4, 151, 200},
    {125.5, 225.4, 201, 300},
    {225.5, 325.4, 301, 500},
};

static const aqi_breakpoint_t PM10_BREAKPOINTS[] = {
    {0.0, 54.0, 0, 50},
    {55.0, 154.0, 51, 100},
    {155.0, 254.0, 101, 150},
    {255.0, 354.0, 151, 200},
    {355.0, 424.0, 201, 300},
    {425.0, 604.0, 301, 500},
};

static double truncate_concentration(double value, int decimals)
{
    const double factor = decimals == 1 ? 10.0 : 1.0;
    return floor(value * factor) / factor;
}

static bool calculate_sub_index(double concentration,
                                int decimals,
                                const aqi_breakpoint_t *breakpoints,
                                size_t breakpoint_count,
                                int *aqi_out)
{
    if (!isfinite(concentration) || concentration < 0.0 || aqi_out == NULL || breakpoint_count == 0) {
        return false;
    }

    double truncated = truncate_concentration(concentration, decimals);
    const aqi_breakpoint_t *bp = &breakpoints[breakpoint_count - 1];
    if (truncated >= breakpoints[0].concentration_low && truncated <= breakpoints[0].concentration_high) {
        bp = &breakpoints[0];
    } else {
        for (size_t i = 1; i < breakpoint_count; ++i) {
            if (truncated <= breakpoints[i].concentration_high) {
                bp = &breakpoints[i];
                break;
            }
        }
    }

    if (truncated > bp->concentration_high) {
        truncated = bp->concentration_high;
    }

    const double scale = ((double)(bp->index_high - bp->index_low)) /
                         (bp->concentration_high - bp->concentration_low);
    const double index = scale * (truncated - bp->concentration_low) + bp->index_low;
    *aqi_out = (int)lround(index);
    return true;
}

static int category_rank(air_quality_category_t category)
{
    switch (category) {
    case AIR_QUALITY_CATEGORY_GOOD:
        return 1;
    case AIR_QUALITY_CATEGORY_MODERATE:
        return 2;
    case AIR_QUALITY_CATEGORY_UNHEALTHY_SENSITIVE:
        return 3;
    case AIR_QUALITY_CATEGORY_UNHEALTHY:
        return 4;
    case AIR_QUALITY_CATEGORY_VERY_UNHEALTHY:
        return 5;
    case AIR_QUALITY_CATEGORY_HAZARDOUS:
        return 6;
    case AIR_QUALITY_CATEGORY_UNKNOWN:
    default:
        return 0;
    }
}

static air_quality_category_t category_from_index(int aqi)
{
    if (aqi <= 50) {
        return AIR_QUALITY_CATEGORY_GOOD;
    }
    if (aqi <= 100) {
        return AIR_QUALITY_CATEGORY_MODERATE;
    }
    if (aqi <= 150) {
        return AIR_QUALITY_CATEGORY_UNHEALTHY_SENSITIVE;
    }
    if (aqi <= 200) {
        return AIR_QUALITY_CATEGORY_UNHEALTHY;
    }
    if (aqi <= 300) {
        return AIR_QUALITY_CATEGORY_VERY_UNHEALTHY;
    }
    return AIR_QUALITY_CATEGORY_HAZARDOUS;
}

void air_quality_compute_us_aqi(const sensor_snapshot_t *snapshot, air_quality_us_aqi_t *result)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->pm2_5_aqi = -1;
    result->pm10_aqi = -1;
    result->category = AIR_QUALITY_CATEGORY_UNKNOWN;
    result->dominant_pollutant = AIR_QUALITY_POLLUTANT_NONE;

    if (snapshot == NULL || !snapshot->pm_valid) {
        return;
    }

    bool pm2_5_valid = calculate_sub_index(snapshot->pm2_5,
                                           1,
                                           PM2_5_BREAKPOINTS,
                                           sizeof(PM2_5_BREAKPOINTS) / sizeof(PM2_5_BREAKPOINTS[0]),
                                           &result->pm2_5_aqi);
    bool pm10_valid = calculate_sub_index(snapshot->pm10_0,
                                          0,
                                          PM10_BREAKPOINTS,
                                          sizeof(PM10_BREAKPOINTS) / sizeof(PM10_BREAKPOINTS[0]),
                                          &result->pm10_aqi);

    if (!pm2_5_valid && !pm10_valid) {
        return;
    }

    if (pm2_5_valid && (!pm10_valid || result->pm2_5_aqi >= result->pm10_aqi)) {
        result->aqi = result->pm2_5_aqi;
        result->dominant_pollutant = AIR_QUALITY_POLLUTANT_PM2_5;
    } else {
        result->aqi = result->pm10_aqi;
        result->dominant_pollutant = AIR_QUALITY_POLLUTANT_PM10;
    }

    result->valid = true;
    result->category = category_from_index(result->aqi);
}

static air_quality_factor_t factor_from_pollutant(air_quality_pollutant_t pollutant)
{
    switch (pollutant) {
    case AIR_QUALITY_POLLUTANT_PM2_5:
        return AIR_QUALITY_FACTOR_PM2_5;
    case AIR_QUALITY_POLLUTANT_PM10:
        return AIR_QUALITY_FACTOR_PM10;
    case AIR_QUALITY_POLLUTANT_NONE:
    default:
        return AIR_QUALITY_FACTOR_NONE;
    }
}

static void append_note(char *buffer, size_t buffer_len, const char *fragment)
{
    if (buffer == NULL || buffer_len == 0 || fragment == NULL || fragment[0] == '\0') {
        return;
    }

    if (buffer[0] != '\0') {
        strlcat(buffer, " ", buffer_len);
    }
    strlcat(buffer, fragment, buffer_len);
}

static double nonnegative(double value)
{
    return value > 0.0 ? value : 0.0;
}

static size_t find_max_index(const double *values, size_t count)
{
    size_t max_index = 0;
    for (size_t i = 1; i < count; ++i) {
        if (values[i] > values[max_index]) {
            max_index = i;
        }
    }
    return max_index;
}

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

void air_quality_compute_particle_insight(const sensor_snapshot_t *snapshot, air_quality_particle_insight_t *result)
{
    static const char *MASS_BANDS[] = {"0.3-1.0 um", "1.0-2.5 um", "2.5-4.0 um", "4.0-10.0 um"};
    static const char *COUNT_BANDS[] = {"0.3-0.5 um", "0.5-1.0 um", "1.0-2.5 um", "2.5-4.0 um", "4.0-10.0 um"};

    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->profile = AIR_QUALITY_PARTICLE_PROFILE_UNAVAILABLE;

    if (snapshot == NULL || !snapshot->pm_valid) {
        return;
    }

    double mass_bins[] = {
        nonnegative(snapshot->pm1_0),
        nonnegative(snapshot->pm2_5 - snapshot->pm1_0),
        nonnegative(snapshot->pm4_0 - snapshot->pm2_5),
        nonnegative(snapshot->pm10_0 - snapshot->pm4_0),
    };
    double count_bins[] = {
        nonnegative(snapshot->particles_0_5um),
        nonnegative(snapshot->particles_1_0um - snapshot->particles_0_5um),
        nonnegative(snapshot->particles_2_5um - snapshot->particles_1_0um),
        nonnegative(snapshot->particles_4_0um - snapshot->particles_2_5um),
        nonnegative(snapshot->particles_10_0um - snapshot->particles_4_0um),
    };

    size_t dominant_mass_index = find_max_index(mass_bins, sizeof(mass_bins) / sizeof(mass_bins[0]));
    size_t dominant_count_index = find_max_index(count_bins, sizeof(count_bins) / sizeof(count_bins[0]));
    strlcpy(result->dominant_mass_band, MASS_BANDS[dominant_mass_index], sizeof(result->dominant_mass_band));
    strlcpy(result->dominant_count_band, COUNT_BANDS[dominant_count_index], sizeof(result->dominant_count_band));

    double total_pm10 = nonnegative(snapshot->pm10_0);
    if (total_pm10 <= 0.0) {
        return;
    }

    double fine_mass = nonnegative(snapshot->pm2_5);
    double coarse_mass = nonnegative(snapshot->pm10_0 - snapshot->pm2_5);
    double fine_ratio = fine_mass / total_pm10;
    double coarse_ratio = coarse_mass / total_pm10;
    bool fine_number_peak = dominant_count_index <= 1;
    bool coarse_number_peak = dominant_count_index >= 3;
    bool coarse_mass_peak = dominant_mass_index >= 2;
    bool fine_typical = snapshot->typical_particle_size_um > 0.0f &&
                        snapshot->typical_particle_size_um <= PARTICLE_TYPICAL_FINE_MAX_UM;
    bool coarse_typical = snapshot->typical_particle_size_um >= PARTICLE_TYPICAL_COARSE_MIN_UM;

    /*
     * EPA/ATSDR use 2.5 um as the fine/coarse split. For interpretation we anchor the
     * profile on the PM2.5 share of PM10, then use the SPS30 size-bin peaks and
     * typical particle size as supporting evidence. The <= 0.35 dust-like threshold is
     * drawn from event-separation literature; >= 0.60 is a conservative engineering
     * cutoff for a fine-dominant episode rather than a formal standard.
     */
    if (fine_ratio >= PARTICLE_FINE_RATIO_VERY_HIGH ||
        (fine_ratio >= PARTICLE_FINE_RATIO_HIGH && fine_number_peak && fine_typical)) {
        result->profile = AIR_QUALITY_PARTICLE_PROFILE_FINE;
    } else if (fine_ratio <= PARTICLE_FINE_TO_PM10_DUST_LIKE_MAX ||
               (coarse_ratio >= PARTICLE_COARSE_RATIO_HIGH &&
                (coarse_mass_peak || coarse_number_peak || coarse_typical))) {
        result->profile = AIR_QUALITY_PARTICLE_PROFILE_COARSE;
    } else {
        result->profile = AIR_QUALITY_PARTICLE_PROFILE_MIXED;
    }

    snprintf(result->note, sizeof(result->note),
             "PM2.5 is %.0f%% of PM10, PM10-2.5 is %.0f%%, mass peaks at %s, counts peak at %s, typical size %.2f um.",
             fine_ratio * 100.0,
             coarse_ratio * 100.0,
             result->dominant_mass_band,
             result->dominant_count_band,
             snapshot->typical_particle_size_um);
    result->valid = true;
}

air_quality_signal_level_t air_quality_rate_co2(uint16_t co2_ppm)
{
    /*
     * CO2 is used here as a ventilation proxy, not a direct pollutant score.
     * 800 ppm is a common "well ventilated" target, while roughly 1000-1200 ppm
     * indicates visitor acceptability for human bioeffluents in typical offices.
     */
    if (co2_ppm <= CO2_GOOD_MAX_PPM) {
        return AIR_QUALITY_SIGNAL_GOOD;
    }
    if (co2_ppm <= CO2_ACCEPTABLE_MAX_PPM) {
        return AIR_QUALITY_SIGNAL_ACCEPTABLE;
    }
    if (co2_ppm <= CO2_ELEVATED_MAX_PPM) {
        return AIR_QUALITY_SIGNAL_ELEVATED;
    }
    if (co2_ppm <= CO2_HIGH_MAX_PPM) {
        return AIR_QUALITY_SIGNAL_HIGH;
    }
    return AIR_QUALITY_SIGNAL_VERY_HIGH;
}

air_quality_signal_level_t air_quality_rate_voc_index(int32_t voc_index)
{
    /*
     * Sensirion maps the recent average VOC condition to 100 and uses >150 as
     * an example action threshold. The 230 threshold is the default gating
     * point in the Gas Index Algorithm. Any split above that remains a UI
     * heuristic for intensity, not an absolute health standard.
     */
    if (voc_index <= 0) {
        return AIR_QUALITY_SIGNAL_UNAVAILABLE;
    }
    if (voc_index <= VOC_GOOD_MAX_INDEX) {
        return AIR_QUALITY_SIGNAL_GOOD;
    }
    if (voc_index <= VOC_ACCEPTABLE_MAX_INDEX) {
        return AIR_QUALITY_SIGNAL_ACCEPTABLE;
    }
    if (voc_index <= VOC_ELEVATED_MAX_INDEX) {
        return AIR_QUALITY_SIGNAL_ELEVATED;
    }
    if (voc_index <= VOC_HIGH_MAX_INDEX) {
        return AIR_QUALITY_SIGNAL_HIGH;
    }
    return AIR_QUALITY_SIGNAL_VERY_HIGH;
}

air_quality_signal_level_t air_quality_rate_nox_index(int32_t nox_index)
{
    /*
     * Sensirion maps the recent average NOx condition to 1 and uses >20 as an
     * example action threshold. The extra splits above that are UI heuristics
     * for event intensity, not absolute concentration standards.
     */
    if (nox_index <= 0) {
        return AIR_QUALITY_SIGNAL_UNAVAILABLE;
    }
    if (nox_index <= NOX_GOOD_MAX_INDEX) {
        return AIR_QUALITY_SIGNAL_GOOD;
    }
    if (nox_index <= NOX_ACCEPTABLE_MAX_INDEX) {
        return AIR_QUALITY_SIGNAL_ACCEPTABLE;
    }
    if (nox_index <= NOX_ELEVATED_MAX_INDEX) {
        return AIR_QUALITY_SIGNAL_ELEVATED;
    }
    if (nox_index <= NOX_HIGH_MAX_INDEX) {
        return AIR_QUALITY_SIGNAL_HIGH;
    }
    return AIR_QUALITY_SIGNAL_VERY_HIGH;
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
    append_note(result->basis, sizeof(result->basis), fragment);

    if (humidity_trend_valid) {
        snprintf(fragment, sizeof(fragment), "RH %+0.1f%%/3h.", snapshot->humidity_trend_rh_3h);
        append_note(result->basis, sizeof(result->basis), fragment);
    } else if (humidity_valid) {
        snprintf(fragment, sizeof(fragment), "RH %.0f%% now.", snapshot->humidity_rh);
        append_note(result->basis, sizeof(result->basis), fragment);
    } else {
        append_note(result->basis, sizeof(result->basis), "Humidity trend unavailable.");
    }

    if (result->dew_point_spread_valid) {
        snprintf(fragment, sizeof(fragment), "Dew-point spread %.1fC.", result->dew_point_spread_c);
        append_note(result->basis, sizeof(result->basis), fragment);
    }
}

const char *air_quality_co2_ventilation_label(air_quality_signal_level_t level)
{
    switch (level) {
    case AIR_QUALITY_SIGNAL_GOOD:
        return "Well Ventilated";
    case AIR_QUALITY_SIGNAL_ACCEPTABLE:
        return "Acceptable Ventilation";
    case AIR_QUALITY_SIGNAL_ELEVATED:
        return "Stale Air";
    case AIR_QUALITY_SIGNAL_HIGH:
        return "Poor Ventilation";
    case AIR_QUALITY_SIGNAL_VERY_HIGH:
        return "Very Poor Ventilation";
    case AIR_QUALITY_SIGNAL_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_voc_event_label(air_quality_signal_level_t level)
{
    switch (level) {
    case AIR_QUALITY_SIGNAL_GOOD:
        return "Below Baseline";
    case AIR_QUALITY_SIGNAL_ACCEPTABLE:
        return "Near Baseline";
    case AIR_QUALITY_SIGNAL_ELEVATED:
        return "VOC Event";
    case AIR_QUALITY_SIGNAL_HIGH:
        return "Strong VOC Event";
    case AIR_QUALITY_SIGNAL_VERY_HIGH:
        return "Severe VOC Event";
    case AIR_QUALITY_SIGNAL_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_nox_event_label(air_quality_signal_level_t level)
{
    switch (level) {
    case AIR_QUALITY_SIGNAL_GOOD:
        return "Background";
    case AIR_QUALITY_SIGNAL_ACCEPTABLE:
        return "Trace NOx Event";
    case AIR_QUALITY_SIGNAL_ELEVATED:
        return "NOx Event";
    case AIR_QUALITY_SIGNAL_HIGH:
        return "High NOx Event";
    case AIR_QUALITY_SIGNAL_VERY_HIGH:
        return "Severe NOx Event";
    case AIR_QUALITY_SIGNAL_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_rate_temperature_label(float temperature_c)
{
    /* Use a broad U.S. indoor comfort band equivalent to roughly 68-79 F. */
    if (!isfinite(temperature_c)) {
        return "Unavailable";
    }
    if (temperature_c < TEMPERATURE_RECOMMENDED_MIN_C) {
        return "Low";
    }
    if (temperature_c <= TEMPERATURE_RECOMMENDED_MAX_C) {
        return "Acceptable";
    }
    return "High";
}

const char *air_quality_rate_humidity_label(float humidity_rh)
{
    if (!isfinite(humidity_rh)) {
        return "Unavailable";
    }
    if (humidity_rh < HUMIDITY_RECOMMENDED_MIN_RH) {
        return "Low";
    }
    if (humidity_rh <= HUMIDITY_RECOMMENDED_MAX_RH) {
        return "Acceptable";
    }
    return "High";
}

const char *air_quality_pressure_trend_label(air_quality_pressure_trend_t trend)
{
    switch (trend) {
    case AIR_QUALITY_PRESSURE_TREND_RISING_FAST:
        return "Rising Fast";
    case AIR_QUALITY_PRESSURE_TREND_RISING:
        return "Rising";
    case AIR_QUALITY_PRESSURE_TREND_STABLE:
        return "Stable";
    case AIR_QUALITY_PRESSURE_TREND_FALLING:
        return "Falling";
    case AIR_QUALITY_PRESSURE_TREND_FALLING_FAST:
        return "Falling Fast";
    case AIR_QUALITY_PRESSURE_TREND_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_pressure_trend_key(air_quality_pressure_trend_t trend)
{
    switch (trend) {
    case AIR_QUALITY_PRESSURE_TREND_RISING_FAST:
        return "rising-fast";
    case AIR_QUALITY_PRESSURE_TREND_RISING:
        return "rising";
    case AIR_QUALITY_PRESSURE_TREND_STABLE:
        return "stable";
    case AIR_QUALITY_PRESSURE_TREND_FALLING:
        return "falling";
    case AIR_QUALITY_PRESSURE_TREND_FALLING_FAST:
        return "falling-fast";
    case AIR_QUALITY_PRESSURE_TREND_UNAVAILABLE:
    default:
        return "unavailable";
    }
}

const char *air_quality_rain_outlook_label(air_quality_rain_outlook_t outlook)
{
    switch (outlook) {
    case AIR_QUALITY_RAIN_OUTLOOK_UNLIKELY:
        return "Unlikely";
    case AIR_QUALITY_RAIN_OUTLOOK_SLIGHT_CHANCE:
        return "Slight Chance";
    case AIR_QUALITY_RAIN_OUTLOOK_POSSIBLE:
        return "Possible";
    case AIR_QUALITY_RAIN_OUTLOOK_LIKELY:
        return "Likely Soon";
    case AIR_QUALITY_RAIN_OUTLOOK_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_rain_outlook_key(air_quality_rain_outlook_t outlook)
{
    switch (outlook) {
    case AIR_QUALITY_RAIN_OUTLOOK_UNLIKELY:
        return "unlikely";
    case AIR_QUALITY_RAIN_OUTLOOK_SLIGHT_CHANCE:
        return "slight";
    case AIR_QUALITY_RAIN_OUTLOOK_POSSIBLE:
        return "possible";
    case AIR_QUALITY_RAIN_OUTLOOK_LIKELY:
        return "likely";
    case AIR_QUALITY_RAIN_OUTLOOK_UNAVAILABLE:
    default:
        return "unavailable";
    }
}

const char *air_quality_rain_season_label(air_quality_rain_season_t season)
{
    switch (season) {
    case AIR_QUALITY_RAIN_SEASON_SPRING:
        return "Spring";
    case AIR_QUALITY_RAIN_SEASON_MEIYU:
        return "Meiyu Season";
    case AIR_QUALITY_RAIN_SEASON_SUMMER:
        return "Summer";
    case AIR_QUALITY_RAIN_SEASON_AUTUMN:
        return "Autumn";
    case AIR_QUALITY_RAIN_SEASON_WINTER:
        return "Winter";
    case AIR_QUALITY_RAIN_SEASON_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_rain_season_key(air_quality_rain_season_t season)
{
    switch (season) {
    case AIR_QUALITY_RAIN_SEASON_SPRING:
        return "spring";
    case AIR_QUALITY_RAIN_SEASON_MEIYU:
        return "meiyu";
    case AIR_QUALITY_RAIN_SEASON_SUMMER:
        return "summer";
    case AIR_QUALITY_RAIN_SEASON_AUTUMN:
        return "autumn";
    case AIR_QUALITY_RAIN_SEASON_WINTER:
        return "winter";
    case AIR_QUALITY_RAIN_SEASON_UNAVAILABLE:
    default:
        return "unavailable";
    }
}

void air_quality_compute_overall_assessment(const sensor_snapshot_t *snapshot, air_quality_assessment_t *result)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->category = AIR_QUALITY_CATEGORY_UNKNOWN;
    result->dominant_factor = AIR_QUALITY_FACTOR_NONE;

    if (snapshot == NULL) {
        strlcpy(result->basis, "No sensor data", sizeof(result->basis));
        return;
    }

    air_quality_compute_us_aqi(snapshot, &result->us_aqi);
    if (result->us_aqi.valid) {
        result->valid = true;
        result->uses_us_aqi = true;
        result->category = result->us_aqi.category;
        result->dominant_factor = factor_from_pollutant(result->us_aqi.dominant_pollutant);
        strlcpy(result->basis, "EPA AQI (PM2.5 / PM10)", sizeof(result->basis));
    } else {
        strlcpy(result->basis, "EPA AQI unavailable", sizeof(result->basis));
    }

    if (snapshot->scd41_valid && isfinite(snapshot->humidity_rh) &&
        (snapshot->humidity_rh < HUMIDITY_RECOMMENDED_MIN_RH ||
         snapshot->humidity_rh > HUMIDITY_RECOMMENDED_MAX_RH)) {
        result->humidity_alert = true;
        if (category_rank(result->category) < category_rank(AIR_QUALITY_CATEGORY_MODERATE)) {
            result->valid = true;
            result->category = AIR_QUALITY_CATEGORY_MODERATE;
            result->dominant_factor = AIR_QUALITY_FACTOR_HUMIDITY;
            if (!result->uses_us_aqi) {
                strlcpy(result->basis, "U.S. indoor humidity guidance", sizeof(result->basis));
            }
        }
    }

    if (snapshot->scd41_valid && snapshot->co2_ppm > CO2_VENTILATION_PROXY_ALERT_PPM) {
        result->co2_ventilation_alert = true;
        if (category_rank(result->category) < category_rank(AIR_QUALITY_CATEGORY_MODERATE)) {
            result->valid = true;
            result->category = AIR_QUALITY_CATEGORY_MODERATE;
            result->dominant_factor = AIR_QUALITY_FACTOR_CO2;
            if (!result->uses_us_aqi) {
                strlcpy(result->basis, "U.S. indoor ventilation proxy", sizeof(result->basis));
            }
        }
    }

    if (!result->uses_us_aqi && snapshot->scd41_valid &&
        !result->co2_ventilation_alert && !result->humidity_alert) {
        result->valid = true;
        result->category = AIR_QUALITY_CATEGORY_GOOD;
        result->dominant_factor = AIR_QUALITY_FACTOR_CO2;
        strlcpy(result->basis, "U.S. indoor ventilation/humidity guidance", sizeof(result->basis));
    }

    result->has_supplemental_only_metrics =
        snapshot->sgp41_voc_valid || snapshot->sgp41_nox_valid || snapshot->pm_valid;

    if (result->uses_us_aqi && (result->co2_ventilation_alert || result->humidity_alert)) {
        strlcpy(result->basis, "EPA AQI with U.S. indoor guidance", sizeof(result->basis));
    }

    if (result->uses_us_aqi && result->category == result->us_aqi.category &&
        !result->co2_ventilation_alert && !result->humidity_alert) {
        append_note(result->note, sizeof(result->note),
                    "Overall rating follows the official EPA AQI based on PM2.5 and PM10.");
    } else if (result->uses_us_aqi && result->category != result->us_aqi.category) {
        append_note(result->note, sizeof(result->note),
                    "PM AQI is lower, but indoor guidance raises the overall rating.");
    } else if (!result->uses_us_aqi && result->valid && snapshot->scd41_valid) {
        append_note(result->note, sizeof(result->note),
                    "PM AQI is unavailable, so the rating is limited to CO2 and humidity guidance.");
    }

    if (result->co2_ventilation_alert) {
        append_note(result->note, sizeof(result->note),
                    "CO2 is above the ventilation proxy threshold (1000 ppm).");
    }

    if (result->humidity_alert) {
        append_note(result->note, sizeof(result->note),
                    "Relative humidity is outside the recommended 30%-60% range.");
    }

    if (snapshot->sgp41_voc_valid || snapshot->sgp41_nox_valid) {
        append_note(result->note, sizeof(result->note),
                    "VOC/NOx indexes are reported separately and are not EPA AQI pollutants.");
    }

    if (snapshot->pm_valid) {
        append_note(result->note, sizeof(result->note),
                    "PM1.0, PM4.0, particle counts and typical particle size are supplemental only.");
    }

    if (!result->valid && result->note[0] == '\0') {
        append_note(result->note, sizeof(result->note),
                    "Not enough AQI-supported measurements are available yet.");
    }
}

const char *air_quality_category_label(air_quality_category_t category)
{
    switch (category) {
    case AIR_QUALITY_CATEGORY_GOOD:
        return "Good";
    case AIR_QUALITY_CATEGORY_MODERATE:
        return "Moderate";
    case AIR_QUALITY_CATEGORY_UNHEALTHY_SENSITIVE:
        return "Unhealthy for Sensitive Groups";
    case AIR_QUALITY_CATEGORY_UNHEALTHY:
        return "Unhealthy";
    case AIR_QUALITY_CATEGORY_VERY_UNHEALTHY:
        return "Very Unhealthy";
    case AIR_QUALITY_CATEGORY_HAZARDOUS:
        return "Hazardous";
    case AIR_QUALITY_CATEGORY_UNKNOWN:
    default:
        return "Unavailable";
    }
}

const char *air_quality_category_key(air_quality_category_t category)
{
    switch (category) {
    case AIR_QUALITY_CATEGORY_GOOD:
        return "good";
    case AIR_QUALITY_CATEGORY_MODERATE:
        return "moderate";
    case AIR_QUALITY_CATEGORY_UNHEALTHY_SENSITIVE:
        return "sensitive";
    case AIR_QUALITY_CATEGORY_UNHEALTHY:
        return "unhealthy";
    case AIR_QUALITY_CATEGORY_VERY_UNHEALTHY:
        return "very-unhealthy";
    case AIR_QUALITY_CATEGORY_HAZARDOUS:
        return "hazardous";
    case AIR_QUALITY_CATEGORY_UNKNOWN:
    default:
        return "unavailable";
    }
}

const char *air_quality_factor_label(air_quality_factor_t factor)
{
    switch (factor) {
    case AIR_QUALITY_FACTOR_PM2_5:
        return "PM2.5";
    case AIR_QUALITY_FACTOR_PM10:
        return "PM10";
    case AIR_QUALITY_FACTOR_CO2:
        return "CO2";
    case AIR_QUALITY_FACTOR_HUMIDITY:
        return "Humidity";
    case AIR_QUALITY_FACTOR_NONE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_signal_level_label(air_quality_signal_level_t level)
{
    switch (level) {
    case AIR_QUALITY_SIGNAL_GOOD:
        return "Good";
    case AIR_QUALITY_SIGNAL_ACCEPTABLE:
        return "Acceptable";
    case AIR_QUALITY_SIGNAL_ELEVATED:
        return "Elevated";
    case AIR_QUALITY_SIGNAL_HIGH:
        return "High";
    case AIR_QUALITY_SIGNAL_VERY_HIGH:
        return "Very High";
    case AIR_QUALITY_SIGNAL_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_signal_level_key(air_quality_signal_level_t level)
{
    switch (level) {
    case AIR_QUALITY_SIGNAL_GOOD:
        return "good";
    case AIR_QUALITY_SIGNAL_ACCEPTABLE:
        return "acceptable";
    case AIR_QUALITY_SIGNAL_ELEVATED:
        return "elevated";
    case AIR_QUALITY_SIGNAL_HIGH:
        return "high";
    case AIR_QUALITY_SIGNAL_VERY_HIGH:
        return "very-high";
    case AIR_QUALITY_SIGNAL_UNAVAILABLE:
    default:
        return "unavailable";
    }
}

const char *air_quality_particle_profile_label(air_quality_particle_profile_t profile)
{
    switch (profile) {
    case AIR_QUALITY_PARTICLE_PROFILE_FINE:
        return "Fine-Mode Dominant";
    case AIR_QUALITY_PARTICLE_PROFILE_MIXED:
        return "Mixed-Mode";
    case AIR_QUALITY_PARTICLE_PROFILE_COARSE:
        return "Coarse-Mode Dominant";
    case AIR_QUALITY_PARTICLE_PROFILE_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_particle_profile_key(air_quality_particle_profile_t profile)
{
    switch (profile) {
    case AIR_QUALITY_PARTICLE_PROFILE_FINE:
        return "fine";
    case AIR_QUALITY_PARTICLE_PROFILE_MIXED:
        return "mixed";
    case AIR_QUALITY_PARTICLE_PROFILE_COARSE:
        return "coarse";
    case AIR_QUALITY_PARTICLE_PROFILE_UNAVAILABLE:
    default:
        return "unavailable";
    }
}

const char *air_quality_pollutant_label(air_quality_pollutant_t pollutant)
{
    switch (pollutant) {
    case AIR_QUALITY_POLLUTANT_PM2_5:
        return "PM2.5";
    case AIR_QUALITY_POLLUTANT_PM10:
        return "PM10";
    case AIR_QUALITY_POLLUTANT_NONE:
    default:
        return "Unavailable";
    }
}
