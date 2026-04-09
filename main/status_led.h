#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef bool (*status_led_enabled_cb_t)(void *user_ctx);

esp_err_t status_led_start(status_led_enabled_cb_t enabled_cb, void *user_ctx);
bool status_led_is_ready(void);
esp_err_t status_led_update_now(void);
