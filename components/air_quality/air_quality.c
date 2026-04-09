#include "air_quality_internal.h"

#include <math.h>
#include <string.h>

int air_quality_category_rank(air_quality_category_t category)
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

air_quality_category_t air_quality_category_from_index(int aqi)
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

air_quality_factor_t air_quality_factor_from_pollutant(air_quality_pollutant_t pollutant)
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
        result->dominant_factor = air_quality_factor_from_pollutant(result->us_aqi.dominant_pollutant);
        strlcpy(result->basis, "EPA AQI (PM2.5 / PM10)", sizeof(result->basis));
    } else {
        strlcpy(result->basis, "EPA AQI unavailable", sizeof(result->basis));
    }

    if (snapshot->scd41_valid && isfinite(snapshot->humidity_rh) &&
        (snapshot->humidity_rh < HUMIDITY_RECOMMENDED_MIN_RH ||
         snapshot->humidity_rh > HUMIDITY_RECOMMENDED_MAX_RH)) {
        result->humidity_alert = true;
        if (air_quality_category_rank(result->category) < air_quality_category_rank(AIR_QUALITY_CATEGORY_MODERATE)) {
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
        if (air_quality_category_rank(result->category) < air_quality_category_rank(AIR_QUALITY_CATEGORY_MODERATE)) {
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
        air_quality_append_note(result->note, sizeof(result->note),
                                "Overall rating follows the official EPA AQI based on PM2.5 and PM10.");
    } else if (result->uses_us_aqi && result->category != result->us_aqi.category) {
        air_quality_append_note(result->note, sizeof(result->note),
                                "PM AQI is lower, but indoor guidance raises the overall rating.");
    } else if (!result->uses_us_aqi && result->valid && snapshot->scd41_valid) {
        air_quality_append_note(result->note, sizeof(result->note),
                                "PM AQI is unavailable, so the rating is limited to CO2 and humidity guidance.");
    }

    if (result->co2_ventilation_alert) {
        air_quality_append_note(result->note, sizeof(result->note),
                                "CO2 is above the ventilation proxy threshold (1000 ppm).");
    }

    if (result->humidity_alert) {
        air_quality_append_note(result->note, sizeof(result->note),
                                "Relative humidity is outside the recommended 30%-60% range.");
    }

    if (snapshot->sgp41_voc_valid || snapshot->sgp41_nox_valid) {
        air_quality_append_note(result->note, sizeof(result->note),
                                "VOC/NOx indexes are reported separately and are not EPA AQI pollutants.");
    }

    if (snapshot->pm_valid) {
        air_quality_append_note(result->note, sizeof(result->note),
                                "PM1.0, PM4.0, particle counts and typical particle size are supplemental only.");
    }

    if (!result->valid && result->note[0] == '\0') {
        air_quality_append_note(result->note, sizeof(result->note),
                                "Not enough AQI-supported measurements are available yet.");
    }
}
