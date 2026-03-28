#pragma once

#include <stdbool.h>

#include "device_types.h"

#define AIR_QUALITY_BASIS_LEN 64
#define AIR_QUALITY_NOTE_LEN 160
#define AIR_QUALITY_PARTICLE_LABEL_LEN 24

typedef enum {
    AIR_QUALITY_CATEGORY_UNKNOWN = 0,
    AIR_QUALITY_CATEGORY_GOOD,
    AIR_QUALITY_CATEGORY_MODERATE,
    AIR_QUALITY_CATEGORY_UNHEALTHY_SENSITIVE,
    AIR_QUALITY_CATEGORY_UNHEALTHY,
    AIR_QUALITY_CATEGORY_VERY_UNHEALTHY,
    AIR_QUALITY_CATEGORY_HAZARDOUS,
} air_quality_category_t;

typedef enum {
    AIR_QUALITY_POLLUTANT_NONE = 0,
    AIR_QUALITY_POLLUTANT_PM2_5,
    AIR_QUALITY_POLLUTANT_PM10,
} air_quality_pollutant_t;

typedef enum {
    AIR_QUALITY_FACTOR_NONE = 0,
    AIR_QUALITY_FACTOR_PM2_5,
    AIR_QUALITY_FACTOR_PM10,
    AIR_QUALITY_FACTOR_CO2,
    AIR_QUALITY_FACTOR_HUMIDITY,
} air_quality_factor_t;

typedef enum {
    AIR_QUALITY_SIGNAL_UNAVAILABLE = 0,
    AIR_QUALITY_SIGNAL_GOOD,
    AIR_QUALITY_SIGNAL_ACCEPTABLE,
    AIR_QUALITY_SIGNAL_ELEVATED,
    AIR_QUALITY_SIGNAL_HIGH,
    AIR_QUALITY_SIGNAL_VERY_HIGH,
} air_quality_signal_level_t;

typedef struct {
    bool valid;
    int aqi;
    int pm2_5_aqi;
    int pm10_aqi;
    air_quality_category_t category;
    air_quality_pollutant_t dominant_pollutant;
} air_quality_us_aqi_t;

typedef struct {
    bool valid;
    bool uses_us_aqi;
    bool co2_ventilation_alert;
    bool humidity_alert;
    bool has_supplemental_only_metrics;
    air_quality_category_t category;
    air_quality_factor_t dominant_factor;
    air_quality_us_aqi_t us_aqi;
    char basis[AIR_QUALITY_BASIS_LEN];
    char note[AIR_QUALITY_NOTE_LEN];
} air_quality_assessment_t;

typedef enum {
    AIR_QUALITY_PARTICLE_PROFILE_UNAVAILABLE = 0,
    AIR_QUALITY_PARTICLE_PROFILE_FINE,
    AIR_QUALITY_PARTICLE_PROFILE_MIXED,
    AIR_QUALITY_PARTICLE_PROFILE_COARSE,
} air_quality_particle_profile_t;

typedef enum {
    AIR_QUALITY_PRESSURE_TREND_UNAVAILABLE = 0,
    AIR_QUALITY_PRESSURE_TREND_RISING_FAST,
    AIR_QUALITY_PRESSURE_TREND_RISING,
    AIR_QUALITY_PRESSURE_TREND_STABLE,
    AIR_QUALITY_PRESSURE_TREND_FALLING,
    AIR_QUALITY_PRESSURE_TREND_FALLING_FAST,
} air_quality_pressure_trend_t;

typedef enum {
    AIR_QUALITY_RAIN_OUTLOOK_UNAVAILABLE = 0,
    AIR_QUALITY_RAIN_OUTLOOK_UNLIKELY,
    AIR_QUALITY_RAIN_OUTLOOK_SLIGHT_CHANCE,
    AIR_QUALITY_RAIN_OUTLOOK_POSSIBLE,
    AIR_QUALITY_RAIN_OUTLOOK_LIKELY,
} air_quality_rain_outlook_t;

typedef enum {
    AIR_QUALITY_RAIN_SEASON_UNAVAILABLE = 0,
    AIR_QUALITY_RAIN_SEASON_SPRING,
    AIR_QUALITY_RAIN_SEASON_MEIYU,
    AIR_QUALITY_RAIN_SEASON_SUMMER,
    AIR_QUALITY_RAIN_SEASON_AUTUMN,
    AIR_QUALITY_RAIN_SEASON_WINTER,
} air_quality_rain_season_t;

typedef struct {
    bool valid;
    air_quality_particle_profile_t profile;
    char dominant_mass_band[AIR_QUALITY_PARTICLE_LABEL_LEN];
    char dominant_count_band[AIR_QUALITY_PARTICLE_LABEL_LEN];
    char note[AIR_QUALITY_NOTE_LEN];
} air_quality_particle_insight_t;

typedef struct {
    bool valid;
    air_quality_pressure_trend_t pressure_trend;
    air_quality_rain_outlook_t outlook;
    air_quality_rain_season_t season;
    bool dew_point_spread_valid;
    float dew_point_spread_c;
    char basis[AIR_QUALITY_NOTE_LEN];
} air_quality_rain_analysis_t;

void air_quality_compute_us_aqi(const sensor_snapshot_t *snapshot, air_quality_us_aqi_t *result);
void air_quality_compute_overall_assessment(const sensor_snapshot_t *snapshot, air_quality_assessment_t *result);
void air_quality_compute_particle_insight(const sensor_snapshot_t *snapshot, air_quality_particle_insight_t *result);
void air_quality_analyze_rain(const sensor_snapshot_t *snapshot, air_quality_rain_analysis_t *result);
air_quality_signal_level_t air_quality_rate_co2(uint16_t co2_ppm);
air_quality_signal_level_t air_quality_rate_voc_index(int32_t voc_index);
air_quality_signal_level_t air_quality_rate_nox_index(int32_t nox_index);
air_quality_pressure_trend_t air_quality_rate_pressure_trend(float pressure_trend_hpa_3h, bool valid);
air_quality_rain_outlook_t air_quality_estimate_rain_outlook(float pressure_hpa,
                                                             float pressure_trend_hpa_3h,
                                                             bool valid);
const char *air_quality_co2_ventilation_label(air_quality_signal_level_t level);
const char *air_quality_voc_event_label(air_quality_signal_level_t level);
const char *air_quality_nox_event_label(air_quality_signal_level_t level);
const char *air_quality_rate_temperature_label(float temperature_c);
const char *air_quality_rate_humidity_label(float humidity_rh);
const char *air_quality_pressure_trend_label(air_quality_pressure_trend_t trend);
const char *air_quality_pressure_trend_key(air_quality_pressure_trend_t trend);
const char *air_quality_rain_outlook_label(air_quality_rain_outlook_t outlook);
const char *air_quality_rain_outlook_key(air_quality_rain_outlook_t outlook);
const char *air_quality_rain_season_label(air_quality_rain_season_t season);
const char *air_quality_rain_season_key(air_quality_rain_season_t season);
const char *air_quality_category_label(air_quality_category_t category);
const char *air_quality_category_key(air_quality_category_t category);
const char *air_quality_pollutant_label(air_quality_pollutant_t pollutant);
const char *air_quality_factor_label(air_quality_factor_t factor);
const char *air_quality_signal_level_label(air_quality_signal_level_t level);
const char *air_quality_signal_level_key(air_quality_signal_level_t level);
const char *air_quality_particle_profile_label(air_quality_particle_profile_t profile);
const char *air_quality_particle_profile_key(air_quality_particle_profile_t profile);
