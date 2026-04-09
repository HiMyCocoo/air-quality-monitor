#include "device_state_json.h"

#include <stdbool.h>
#include <stdint.h>

#include "air_quality.h"
#include "esp_timer.h"

typedef struct {
    const char *co2_key;
    const char *temperature_key;
    const char *humidity_key;
    const char *bmp390_temperature_key;
    const char *pressure_key;
} device_state_json_key_map_t;

static const device_state_json_key_map_t MQTT_KEY_MAP = {
    .co2_key = "co2",
    .temperature_key = "temperature",
    .humidity_key = "humidity",
    .bmp390_temperature_key = "bmp390_temperature",
    .pressure_key = "pressure",
};

static const device_state_json_key_map_t WEB_KEY_MAP = {
    .co2_key = "co2_ppm",
    .temperature_key = "temperature_c",
    .humidity_key = "humidity_rh",
    .bmp390_temperature_key = "bmp390_temperature_c",
    .pressure_key = "pressure_hpa",
};

static const device_state_json_key_map_t *device_state_json_keys(device_state_json_profile_t profile)
{
    return profile == DEVICE_STATE_JSON_PROFILE_WEB ? &WEB_KEY_MAP : &MQTT_KEY_MAP;
}

static void add_number_or_null(cJSON *json, const char *key, bool valid, double value)
{
    if (valid) {
        cJSON_AddNumberToObject(json, key, value);
    } else {
        cJSON_AddNullToObject(json, key);
    }
}

esp_err_t device_state_json_build_sensor_state(cJSON *json,
                                               const sensor_snapshot_t *snapshot,
                                               device_state_json_profile_t profile,
                                               const device_state_json_options_t *options)
{
    if (json == NULL || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const device_state_json_options_t defaults = {0};
    if (options == NULL) {
        options = &defaults;
    }

    const device_state_json_key_map_t *keys = device_state_json_keys(profile);
    air_quality_assessment_t assessment = {0};
    air_quality_compute_overall_assessment(snapshot, &assessment);
    air_quality_particle_insight_t particle = {0};
    air_quality_compute_particle_insight(snapshot, &particle);
    air_quality_rain_analysis_t rain = {0};
    air_quality_analyze_rain(snapshot, &rain);

    cJSON_AddBoolToObject(json, "scd41_valid", snapshot->scd41_valid);
    cJSON_AddBoolToObject(json, "sgp41_valid", snapshot->sgp41_valid);
    cJSON_AddBoolToObject(json, "sgp41_conditioning", snapshot->sgp41_conditioning);
    cJSON_AddBoolToObject(json, "sgp41_voc_valid", snapshot->sgp41_voc_valid);
    cJSON_AddBoolToObject(json, "sgp41_nox_valid", snapshot->sgp41_nox_valid);
    cJSON_AddBoolToObject(json, "bmp390_valid", snapshot->bmp390_valid);
    cJSON_AddBoolToObject(json, "pm_valid", snapshot->pm_valid);
    cJSON_AddStringToObject(json, "co2_compensation_source",
                            co2_compensation_source_key(snapshot->co2_compensation_source));
    cJSON_AddNumberToObject(json, "sgp41_voc_stabilization_remaining_s", snapshot->sgp41_voc_stabilization_remaining_s);
    cJSON_AddNumberToObject(json, "sgp41_nox_stabilization_remaining_s", snapshot->sgp41_nox_stabilization_remaining_s);

    if (snapshot->scd41_valid) {
        cJSON_AddNumberToObject(json, keys->co2_key, snapshot->co2_ppm);
        cJSON_AddStringToObject(json, "co2_rating",
                                air_quality_co2_ventilation_label(air_quality_rate_co2(snapshot->co2_ppm)));
        cJSON_AddNumberToObject(json, keys->temperature_key, snapshot->temperature_c);
        cJSON_AddStringToObject(json, "temperature_rating", air_quality_rate_temperature_label(snapshot->temperature_c));
        cJSON_AddNumberToObject(json, keys->humidity_key, snapshot->humidity_rh);
        cJSON_AddStringToObject(json, "humidity_rating", air_quality_rate_humidity_label(snapshot->humidity_rh));
    } else {
        cJSON_AddNullToObject(json, keys->co2_key);
        cJSON_AddNullToObject(json, "co2_rating");
        cJSON_AddNullToObject(json, keys->temperature_key);
        cJSON_AddNullToObject(json, "temperature_rating");
        cJSON_AddNullToObject(json, keys->humidity_key);
        cJSON_AddNullToObject(json, "humidity_rating");
    }

    add_number_or_null(json, "humidity_trend_rh_3h", snapshot->humidity_trend_valid, snapshot->humidity_trend_rh_3h);
    add_number_or_null(json, "humidity_trend_span_min", snapshot->humidity_trend_valid, snapshot->humidity_trend_span_min);

    if (snapshot->sgp41_voc_valid) {
        cJSON_AddNumberToObject(json, "voc_index", snapshot->voc_index);
        cJSON_AddStringToObject(json, "voc_rating",
                                air_quality_voc_event_label(air_quality_rate_voc_index(snapshot->voc_index)));
    } else {
        cJSON_AddNullToObject(json, "voc_index");
        cJSON_AddNullToObject(json, "voc_rating");
    }

    if (snapshot->sgp41_nox_valid) {
        cJSON_AddNumberToObject(json, "nox_index", snapshot->nox_index);
        cJSON_AddStringToObject(json, "nox_rating",
                                air_quality_nox_event_label(air_quality_rate_nox_index(snapshot->nox_index)));
    } else {
        cJSON_AddNullToObject(json, "nox_index");
        cJSON_AddNullToObject(json, "nox_rating");
    }

    if (snapshot->bmp390_valid) {
        cJSON_AddNumberToObject(json, keys->bmp390_temperature_key, snapshot->bmp390_temperature_c);
        cJSON_AddNumberToObject(json, keys->pressure_key, snapshot->pressure_hpa);
    } else {
        cJSON_AddNullToObject(json, keys->bmp390_temperature_key);
        cJSON_AddNullToObject(json, keys->pressure_key);
    }

    add_number_or_null(json, "pressure_trend_hpa_3h", snapshot->pressure_trend_valid, snapshot->pressure_trend_hpa_3h);
    add_number_or_null(json, "pressure_trend_span_min", snapshot->pressure_trend_valid, snapshot->pressure_trend_span_min);
    add_number_or_null(json, "dew_point_spread_c", rain.dew_point_spread_valid, rain.dew_point_spread_c);
    cJSON_AddStringToObject(json, "pressure_trend", air_quality_pressure_trend_label(rain.pressure_trend));
    cJSON_AddStringToObject(json, "pressure_trend_key", air_quality_pressure_trend_key(rain.pressure_trend));
    cJSON_AddStringToObject(json, "rain_outlook", air_quality_rain_outlook_label(rain.outlook));
    cJSON_AddStringToObject(json, "rain_outlook_key", air_quality_rain_outlook_key(rain.outlook));
    cJSON_AddStringToObject(json, "rain_season", air_quality_rain_season_label(rain.season));
    cJSON_AddStringToObject(json, "rain_season_key", air_quality_rain_season_key(rain.season));
    cJSON_AddStringToObject(json, "rain_basis", rain.basis[0] ? rain.basis : "Unavailable");

    if (snapshot->pm_valid) {
        cJSON_AddNumberToObject(json, "pm1_0", snapshot->pm1_0);
        cJSON_AddNumberToObject(json, "pm2_5", snapshot->pm2_5);
        cJSON_AddNumberToObject(json, "pm4_0", snapshot->pm4_0);
        cJSON_AddNumberToObject(json, "pm10_0", snapshot->pm10_0);
        if (particle.valid) {
            cJSON_AddStringToObject(json, "particle_profile", air_quality_particle_profile_label(particle.profile));
            cJSON_AddStringToObject(json, "particle_situation", air_quality_particle_situation_label(particle.situation));
            cJSON_AddStringToObject(json, "particle_profile_note", particle.note[0] ? particle.note : "Unavailable");
            cJSON_AddStringToObject(json, "particle_advice", particle.advice[0] ? particle.advice : "Unavailable");
            cJSON_AddNumberToObject(json, "particle_fine_share_pct", particle.fine_share_pct);
            cJSON_AddNumberToObject(json, "particle_coarse_share_pct", particle.coarse_share_pct);
            cJSON_AddStringToObject(json, "particle_dominant_mass_band", particle.dominant_mass_band);
            cJSON_AddStringToObject(json, "particle_dominant_count_band", particle.dominant_count_band);
            cJSON_AddStringToObject(json, "particle_situation_key",
                                    air_quality_particle_situation_key(particle.situation));
            cJSON_AddStringToObject(json, "particle_profile_key", air_quality_particle_profile_key(particle.profile));
        } else {
            cJSON_AddNullToObject(json, "particle_profile");
            cJSON_AddNullToObject(json, "particle_situation");
            cJSON_AddNullToObject(json, "particle_profile_note");
            cJSON_AddNullToObject(json, "particle_advice");
            cJSON_AddNullToObject(json, "particle_fine_share_pct");
            cJSON_AddNullToObject(json, "particle_coarse_share_pct");
            cJSON_AddNullToObject(json, "particle_dominant_mass_band");
            cJSON_AddNullToObject(json, "particle_dominant_count_band");
            cJSON_AddStringToObject(json, "particle_situation_key",
                                    air_quality_particle_situation_key(AIR_QUALITY_PARTICLE_SITUATION_UNAVAILABLE));
            cJSON_AddStringToObject(json, "particle_profile_key",
                                    air_quality_particle_profile_key(AIR_QUALITY_PARTICLE_PROFILE_UNAVAILABLE));
        }
        cJSON_AddNumberToObject(json, "particles_0_5um", snapshot->particles_0_5um);
        cJSON_AddNumberToObject(json, "particles_1_0um", snapshot->particles_1_0um);
        cJSON_AddNumberToObject(json, "particles_2_5um", snapshot->particles_2_5um);
        cJSON_AddNumberToObject(json, "particles_4_0um", snapshot->particles_4_0um);
        cJSON_AddNumberToObject(json, "particles_10_0um", snapshot->particles_10_0um);
        cJSON_AddNumberToObject(json, "typical_particle_size_um", snapshot->typical_particle_size_um);
    } else {
        cJSON_AddNullToObject(json, "pm1_0");
        cJSON_AddNullToObject(json, "pm2_5");
        cJSON_AddNullToObject(json, "pm4_0");
        cJSON_AddNullToObject(json, "pm10_0");
        cJSON_AddNullToObject(json, "particle_profile");
        cJSON_AddNullToObject(json, "particle_situation");
        cJSON_AddNullToObject(json, "particle_profile_note");
        cJSON_AddNullToObject(json, "particle_advice");
        cJSON_AddNullToObject(json, "particle_fine_share_pct");
        cJSON_AddNullToObject(json, "particle_coarse_share_pct");
        cJSON_AddNullToObject(json, "particle_dominant_mass_band");
        cJSON_AddNullToObject(json, "particle_dominant_count_band");
        cJSON_AddStringToObject(json, "particle_situation_key",
                                air_quality_particle_situation_key(AIR_QUALITY_PARTICLE_SITUATION_UNAVAILABLE));
        cJSON_AddStringToObject(json, "particle_profile_key",
                                air_quality_particle_profile_key(AIR_QUALITY_PARTICLE_PROFILE_UNAVAILABLE));
        cJSON_AddNullToObject(json, "particles_0_5um");
        cJSON_AddNullToObject(json, "particles_1_0um");
        cJSON_AddNullToObject(json, "particles_2_5um");
        cJSON_AddNullToObject(json, "particles_4_0um");
        cJSON_AddNullToObject(json, "particles_10_0um");
        cJSON_AddNullToObject(json, "typical_particle_size_um");
    }

    cJSON_AddBoolToObject(json, "sps30_sleeping", snapshot->sps30_sleeping);

    if (assessment.us_aqi.valid) {
        cJSON_AddNumberToObject(json, "us_aqi", assessment.us_aqi.aqi);
        cJSON_AddStringToObject(json, "us_aqi_level", air_quality_category_label(assessment.us_aqi.category));
        cJSON_AddStringToObject(json, "us_aqi_level_key", air_quality_category_key(assessment.us_aqi.category));
        cJSON_AddStringToObject(json, "us_aqi_primary_pollutant",
                                air_quality_pollutant_label(assessment.us_aqi.dominant_pollutant));
    } else {
        cJSON_AddNullToObject(json, "us_aqi");
        cJSON_AddNullToObject(json, "us_aqi_level");
        cJSON_AddStringToObject(json, "us_aqi_level_key", air_quality_category_key(AIR_QUALITY_CATEGORY_UNKNOWN));
        cJSON_AddNullToObject(json, "us_aqi_primary_pollutant");
    }

    if (assessment.valid) {
        cJSON_AddStringToObject(json, "overall_air_quality", air_quality_category_label(assessment.category));
        cJSON_AddStringToObject(json, "overall_air_quality_key", air_quality_category_key(assessment.category));
        cJSON_AddStringToObject(json, "overall_air_quality_driver",
                                air_quality_factor_label(assessment.dominant_factor));
    } else {
        cJSON_AddNullToObject(json, "overall_air_quality");
        cJSON_AddStringToObject(json, "overall_air_quality_key", air_quality_category_key(AIR_QUALITY_CATEGORY_UNKNOWN));
        cJSON_AddNullToObject(json, "overall_air_quality_driver");
    }

    cJSON_AddStringToObject(json, "overall_air_quality_basis", assessment.basis[0] ? assessment.basis : "Unavailable");
    cJSON_AddStringToObject(json, "overall_air_quality_note", assessment.note[0] ? assessment.note : "Unavailable");

    if (options->include_sample_age_sec) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool sample_age_valid = snapshot->updated_at_ms > 0 && now_ms >= snapshot->updated_at_ms;
        int64_t sample_age_sec = sample_age_valid ? (now_ms - snapshot->updated_at_ms) / 1000 : 0;
        add_number_or_null(json, "sample_age_sec", sample_age_valid, (double)sample_age_sec);
    }

    if (options->include_control_state) {
        cJSON_AddBoolToObject(json, "scd41_asc_enabled", options->scd41_asc_enabled);
        cJSON_AddBoolToObject(json, "status_led_enabled", options->status_led_enabled);
        cJSON_AddNumberToObject(json, "scd41_frc_reference_ppm", options->scd41_frc_reference_ppm);
    }

    return ESP_OK;
}
