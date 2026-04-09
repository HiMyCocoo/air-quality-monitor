#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "air_quality.h"

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
#define PARTICLE_BACKGROUND_PM2_5_MAX 12.0f
#define PARTICLE_BACKGROUND_PM10_MAX 30.0f
#define PARTICLE_DRY_AIR_MAX_RH 35.0f
#define PARTICLE_HUMID_AIR_MIN_RH 60.0f
#define PARTICLE_WARM_ROOM_MIN_C 27.0f
#define PARTICLE_COOL_ROOM_MAX_C 18.0f

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

static inline void air_quality_append_note(char *buffer, size_t buffer_len, const char *fragment)
{
    if (buffer == NULL || buffer_len == 0 || fragment == NULL || fragment[0] == '\0') {
        return;
    }

    if (buffer[0] != '\0') {
        strlcat(buffer, " ", buffer_len);
    }
    strlcat(buffer, fragment, buffer_len);
}

int air_quality_category_rank(air_quality_category_t category);
air_quality_category_t air_quality_category_from_index(int aqi);
air_quality_factor_t air_quality_factor_from_pollutant(air_quality_pollutant_t pollutant);
