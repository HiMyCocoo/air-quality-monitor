#include "air_quality_internal.h"

#include <math.h>

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

const char *air_quality_particle_situation_label(air_quality_particle_situation_t situation)
{
    switch (situation) {
    case AIR_QUALITY_PARTICLE_SITUATION_BACKGROUND:
        return "Clean Background";
    case AIR_QUALITY_PARTICLE_SITUATION_FINE_SOURCE:
        return "Fine Particle Source Event";
    case AIR_QUALITY_PARTICLE_SITUATION_FINE_STALE:
        return "Fine Particle Build-up";
    case AIR_QUALITY_PARTICLE_SITUATION_COARSE_DUST:
        return "Dust / Coarse Disturbance";
    case AIR_QUALITY_PARTICLE_SITUATION_MIXED_ACTIVITY:
        return "Mixed Activity Episode";
    case AIR_QUALITY_PARTICLE_SITUATION_UNAVAILABLE:
    default:
        return "Unavailable";
    }
}

const char *air_quality_particle_situation_key(air_quality_particle_situation_t situation)
{
    switch (situation) {
    case AIR_QUALITY_PARTICLE_SITUATION_BACKGROUND:
        return "background";
    case AIR_QUALITY_PARTICLE_SITUATION_FINE_SOURCE:
        return "fine-source";
    case AIR_QUALITY_PARTICLE_SITUATION_FINE_STALE:
        return "fine-stale";
    case AIR_QUALITY_PARTICLE_SITUATION_COARSE_DUST:
        return "coarse-dust";
    case AIR_QUALITY_PARTICLE_SITUATION_MIXED_ACTIVITY:
        return "mixed-activity";
    case AIR_QUALITY_PARTICLE_SITUATION_UNAVAILABLE:
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
