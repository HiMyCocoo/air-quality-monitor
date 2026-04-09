#include "air_quality_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

static bool finite_positive(float value)
{
    return isfinite(value) && value > 0.0f;
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
    result->situation = AIR_QUALITY_PARTICLE_SITUATION_UNAVAILABLE;

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
    result->fine_share_pct = (float)(fine_ratio * 100.0);
    result->coarse_share_pct = (float)(coarse_ratio * 100.0);
    bool fine_number_peak = dominant_count_index <= 1;
    bool coarse_number_peak = dominant_count_index >= 3;
    bool coarse_mass_peak = dominant_mass_index >= 2;
    bool fine_typical = snapshot->typical_particle_size_um > 0.0f &&
                        snapshot->typical_particle_size_um <= PARTICLE_TYPICAL_FINE_MAX_UM;
    bool coarse_typical = snapshot->typical_particle_size_um >= PARTICLE_TYPICAL_COARSE_MIN_UM;

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

    bool co2_valid = snapshot->scd41_valid && snapshot->co2_ppm > 0;
    bool ventilation_limited = co2_valid && snapshot->co2_ppm > CO2_ACCEPTABLE_MAX_PPM;
    bool humidity_valid = snapshot->scd41_valid && isfinite(snapshot->humidity_rh);
    bool dry_air = humidity_valid && snapshot->humidity_rh < PARTICLE_DRY_AIR_MAX_RH;
    bool humid_air = humidity_valid && snapshot->humidity_rh > PARTICLE_HUMID_AIR_MIN_RH;
    bool temperature_valid = snapshot->scd41_valid && isfinite(snapshot->temperature_c);
    bool warm_room = temperature_valid && snapshot->temperature_c >= PARTICLE_WARM_ROOM_MIN_C;
    bool cool_room = temperature_valid && snapshot->temperature_c <= PARTICLE_COOL_ROOM_MAX_C;
    bool background_pm = finite_positive(snapshot->pm10_0) &&
                         snapshot->pm2_5 <= PARTICLE_BACKGROUND_PM2_5_MAX &&
                         snapshot->pm10_0 <= PARTICLE_BACKGROUND_PM10_MAX &&
                         !ventilation_limited;

    if (background_pm && !dry_air && !humid_air) {
        result->situation = AIR_QUALITY_PARTICLE_SITUATION_BACKGROUND;
    } else if (result->profile == AIR_QUALITY_PARTICLE_PROFILE_FINE) {
        result->situation = ventilation_limited
                                ? AIR_QUALITY_PARTICLE_SITUATION_FINE_STALE
                                : AIR_QUALITY_PARTICLE_SITUATION_FINE_SOURCE;
    } else if (result->profile == AIR_QUALITY_PARTICLE_PROFILE_COARSE) {
        result->situation = AIR_QUALITY_PARTICLE_SITUATION_COARSE_DUST;
    } else {
        result->situation = AIR_QUALITY_PARTICLE_SITUATION_MIXED_ACTIVITY;
    }

    snprintf(result->note, sizeof(result->note),
             "PM2.5 share %.0f%%, coarse share %.0f%%, mass peaks at %s, counts peak at %s, typical size %.2f um.",
             result->fine_share_pct,
             result->coarse_share_pct,
             result->dominant_mass_band,
             result->dominant_count_band,
             snapshot->typical_particle_size_um);
    if (ventilation_limited) {
        char fragment[64];
        snprintf(fragment, sizeof(fragment), "CO2 at %u ppm suggests weak fresh-air exchange.", snapshot->co2_ppm);
        air_quality_append_note(result->note, sizeof(result->note), fragment);
    } else if (co2_valid && snapshot->co2_ppm <= CO2_GOOD_MAX_PPM &&
               result->profile == AIR_QUALITY_PARTICLE_PROFILE_FINE) {
        air_quality_append_note(result->note, sizeof(result->note),
                                "CO2 is still low, so this looks more like an active particle source than stale air alone.");
    }
    if (dry_air) {
        char fragment[64];
        snprintf(fragment, sizeof(fragment), "RH %.0f%% is dry enough for dust to resuspend easily.", snapshot->humidity_rh);
        air_quality_append_note(result->note, sizeof(result->note), fragment);
    } else if (humid_air && result->profile == AIR_QUALITY_PARTICLE_PROFILE_FINE) {
        char fragment[72];
        snprintf(fragment, sizeof(fragment), "RH %.0f%% is high enough to make fine aerosols feel muggy.", snapshot->humidity_rh);
        air_quality_append_note(result->note, sizeof(result->note), fragment);
    }

    switch (result->situation) {
    case AIR_QUALITY_PARTICLE_SITUATION_BACKGROUND:
        strlcpy(result->advice,
                "Particle load is currently low. Keep routine ventilation and cleaning; no extra intervention is needed right now.",
                sizeof(result->advice));
        break;
    case AIR_QUALITY_PARTICLE_SITUATION_FINE_SOURCE:
        strlcpy(result->advice,
                "Fine particles point to a source event. Use the range hood or purifier, stop candles/incense, and avoid opening windows if outdoor haze or smoke is suspected.",
                sizeof(result->advice));
        break;
    case AIR_QUALITY_PARTICLE_SITUATION_FINE_STALE:
        strlcpy(result->advice,
                "Fine particles are lingering in weak air exchange. Do a short cross-ventilation flush or start exhaust now, and keep the range hood running longer after cooking.",
                sizeof(result->advice));
        break;
    case AIR_QUALITY_PARTICLE_SITUATION_COARSE_DUST:
        strlcpy(result->advice,
                "Dust-like coarse particles dominate. Prefer damp wiping or a HEPA vacuum over dry sweeping, reduce shaking fabrics indoors, and close windows during dusty or windy periods.",
                sizeof(result->advice));
        break;
    case AIR_QUALITY_PARTICLE_SITUATION_MIXED_ACTIVITY:
        strlcpy(result->advice,
                "Fine and coarse particles are elevated together. Address both source and air exchange: ventilate briefly if outdoor air is cleaner, then clean floors and surfaces to cut re-suspended dust.",
                sizeof(result->advice));
        break;
    case AIR_QUALITY_PARTICLE_SITUATION_UNAVAILABLE:
    default:
        break;
    }

    if (dry_air && result->situation == AIR_QUALITY_PARTICLE_SITUATION_COARSE_DUST) {
        air_quality_append_note(result->advice, sizeof(result->advice),
                                "If comfortable, raising humidity toward 40%-50% can also reduce dust re-entrainment.");
    } else if (humid_air && result->profile == AIR_QUALITY_PARTICLE_PROFILE_FINE) {
        air_quality_append_note(result->advice, sizeof(result->advice),
                                "If the room feels muggy, dehumidifying toward 40%-60% can improve comfort.");
    }
    if (warm_room && (result->situation == AIR_QUALITY_PARTICLE_SITUATION_FINE_STALE ||
                      result->situation == AIR_QUALITY_PARTICLE_SITUATION_MIXED_ACTIVITY)) {
        air_quality_append_note(result->advice, sizeof(result->advice),
                                "The room is also warm and stuffy, so a short purge ventilation is especially worthwhile.");
    } else if (cool_room && result->situation == AIR_QUALITY_PARTICLE_SITUATION_BACKGROUND) {
        air_quality_append_note(result->advice, sizeof(result->advice),
                                "Because the room is already on the cool side, avoid over-ventilating for too long.");
    }
    result->valid = true;
}
