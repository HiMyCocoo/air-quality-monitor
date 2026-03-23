#include "air_quality.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    double concentration_low;
    double concentration_high;
    int index_low;
    int index_high;
} aqi_breakpoint_t;

#define CO2_GOOD_MAX_PPM 800U
#define CO2_ACCEPTABLE_MAX_PPM 1000U
#define CO2_ELEVATED_MAX_PPM 1500U
#define CO2_HIGH_MAX_PPM 2000U
#define CO2_VENTILATION_PROXY_ALERT_PPM CO2_ACCEPTABLE_MAX_PPM
#define HUMIDITY_RECOMMENDED_MIN_RH 30.0
#define HUMIDITY_RECOMMENDED_MAX_RH 60.0
#define VOC_GOOD_MAX_INDEX 100
#define VOC_ACCEPTABLE_MAX_INDEX 150
#define VOC_ELEVATED_MAX_INDEX 250
#define VOC_HIGH_MAX_INDEX 400
#define NOX_GOOD_MAX_INDEX 1
#define NOX_ACCEPTABLE_MAX_INDEX 10
#define NOX_ELEVATED_MAX_INDEX 20
#define NOX_HIGH_MAX_INDEX 100

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

void air_quality_compute_particle_insight(const sensor_snapshot_t *snapshot, air_quality_particle_insight_t *result)
{
    static const char *MASS_BANDS[] = {"0-1 um", "1-2.5 um", "2.5-4 um", "4-10 um"};
    static const char *COUNT_BANDS[] = {"0.5-1.0 um", "1.0-2.5 um", "2.5-4.0 um", "4.0-10 um", ">10 um"};

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
        nonnegative(snapshot->particles_0_5um - snapshot->particles_1_0um),
        nonnegative(snapshot->particles_1_0um - snapshot->particles_2_5um),
        nonnegative(snapshot->particles_2_5um - snapshot->particles_4_0um),
        nonnegative(snapshot->particles_4_0um - snapshot->particles_10_0um),
        nonnegative(snapshot->particles_10_0um),
    };

    size_t dominant_mass_index = find_max_index(mass_bins, sizeof(mass_bins) / sizeof(mass_bins[0]));
    size_t dominant_count_index = find_max_index(count_bins, sizeof(count_bins) / sizeof(count_bins[0]));
    strlcpy(result->dominant_mass_band, MASS_BANDS[dominant_mass_index], sizeof(result->dominant_mass_band));
    strlcpy(result->dominant_count_band, COUNT_BANDS[dominant_count_index], sizeof(result->dominant_count_band));

    double total_pm10 = nonnegative(snapshot->pm10_0);
    double fine_mass = nonnegative(snapshot->pm2_5);
    double coarse_mass = nonnegative(snapshot->pm10_0 - snapshot->pm2_5);
    double fine_ratio = total_pm10 > 0.0 ? fine_mass / total_pm10 : 0.0;
    double coarse_ratio = total_pm10 > 0.0 ? coarse_mass / total_pm10 : 0.0;

    if (fine_ratio >= 0.75 && snapshot->typical_particle_size_um <= 1.2f) {
        result->profile = AIR_QUALITY_PARTICLE_PROFILE_FINE;
    } else if (coarse_ratio >= 0.50 || snapshot->typical_particle_size_um >= 2.5f || dominant_count_index >= 3) {
        result->profile = AIR_QUALITY_PARTICLE_PROFILE_COARSE;
    } else {
        result->profile = AIR_QUALITY_PARTICLE_PROFILE_MIXED;
    }

    snprintf(result->note, sizeof(result->note),
             "Mass is led by %s, counts are led by %s, typical size %.2f um.",
             result->dominant_mass_band,
             result->dominant_count_band,
             snapshot->typical_particle_size_um);
    result->valid = true;
}

air_quality_signal_level_t air_quality_rate_co2(uint16_t co2_ppm)
{
    /* CO2 is used here as a ventilation proxy for a user-facing comfort rating. */
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
    /* Sensirion maps typical VOC conditions to 100 and uses >150 as a clear event. */
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
    /* Sensirion maps typical NOx conditions to 1 and uses >20 as a clear event. */
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
        (snapshot->sgp41_valid && !snapshot->sgp41_conditioning) || snapshot->pm_valid;

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

    if (snapshot->sgp41_valid && !snapshot->sgp41_conditioning) {
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
        return "Fine-Dominant";
    case AIR_QUALITY_PARTICLE_PROFILE_MIXED:
        return "Mixed";
    case AIR_QUALITY_PARTICLE_PROFILE_COARSE:
        return "Coarse-Dominant";
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
