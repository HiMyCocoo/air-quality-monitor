#include "air_quality.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    double concentration_low;
    double concentration_high;
    int index_low;
    int index_high;
} aqi_breakpoint_t;

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
