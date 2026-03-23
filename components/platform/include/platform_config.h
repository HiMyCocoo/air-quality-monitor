#pragma once

#include <stdbool.h>

#include "device_types.h"
#include "esp_err.h"

esp_err_t platform_config_init(void);
void platform_config_apply_defaults(device_config_t *config, const char *device_id);
void platform_config_sanitize(device_config_t *config, const char *device_id);
bool platform_config_has_wifi(const device_config_t *config);
bool platform_config_has_mqtt(const device_config_t *config);
bool platform_config_is_complete(const device_config_t *config);
esp_err_t platform_config_load(device_config_t *config, const char *device_id);
esp_err_t platform_config_save(const device_config_t *config);
esp_err_t platform_config_reset(void);
