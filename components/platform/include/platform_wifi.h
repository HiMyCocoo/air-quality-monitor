#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "device_types.h"
#include "esp_err.h"

typedef void (*platform_wifi_event_cb_t)(bool connected, void *user_ctx);

esp_err_t platform_wifi_init(platform_wifi_event_cb_t cb, void *user_ctx);
void platform_wifi_build_ap_ssid(char *buffer, size_t buffer_len);
esp_err_t platform_wifi_start_ap(const char *ssid);
esp_err_t platform_wifi_start_sta(const device_config_t *config, int max_retry, int timeout_ms);
esp_err_t platform_wifi_switch_to_ap(const char *ssid);
void platform_wifi_stop(void);
bool platform_wifi_is_connected(void);
platform_wifi_mode_t platform_wifi_get_mode(void);
int platform_wifi_get_rssi(void);
esp_err_t platform_wifi_get_ip(char *buffer, size_t buffer_len);
