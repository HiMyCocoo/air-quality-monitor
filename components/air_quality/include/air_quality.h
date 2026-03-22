#pragma once

#include <stdbool.h>

#include "device_types.h"

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

typedef struct {
    bool valid;
    int aqi;
    int pm2_5_aqi;
    int pm10_aqi;
    air_quality_category_t category;
    air_quality_pollutant_t dominant_pollutant;
} air_quality_us_aqi_t;

void air_quality_compute_us_aqi(const sensor_snapshot_t *snapshot, air_quality_us_aqi_t *result);
const char *air_quality_category_label(air_quality_category_t category);
const char *air_quality_category_key(air_quality_category_t category);
const char *air_quality_pollutant_label(air_quality_pollutant_t pollutant);
