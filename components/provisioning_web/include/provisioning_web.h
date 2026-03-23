#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_types.h"
#include "esp_err.h"

typedef struct {
    void (*get_status)(sensor_snapshot_t *snapshot, device_diag_t *diag, device_config_t *config, uint16_t *frc_ppm, void *user_ctx);
    esp_err_t (*save_config)(const device_config_t *config, bool *restart_required, bool *runtime_applied, void *user_ctx);
    void (*request_restart)(void *user_ctx);
    void (*request_factory_reset)(void *user_ctx);
    esp_err_t (*request_republish_discovery)(void *user_ctx);
    esp_err_t (*request_set_scd41_asc)(bool enabled, void *user_ctx);
    esp_err_t (*request_set_sps30_sleep)(bool sleep, void *user_ctx);
    esp_err_t (*request_set_status_led)(bool enabled, void *user_ctx);
    esp_err_t (*request_apply_frc)(uint16_t ppm, void *user_ctx);
} provisioning_web_callbacks_t;

esp_err_t provisioning_web_start(const char *device_id,
                                 const provisioning_web_callbacks_t *callbacks,
                                 void *user_ctx);
void provisioning_web_stop(void);
