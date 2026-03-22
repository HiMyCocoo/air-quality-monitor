#include "platform_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1

static const char *TAG = "platform_wifi";

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static platform_wifi_mode_t s_mode = PLATFORM_WIFI_MODE_OFF;
static int s_retry_count;
static int s_retry_limit;
static bool s_connected;
static platform_wifi_event_cb_t s_event_cb;
static void *s_event_ctx;
static esp_event_handler_instance_t s_wifi_handler_instance;
static esp_event_handler_instance_t s_ip_handler_instance;

static void notify_state(bool connected)
{
    s_connected = connected;
    if (s_event_cb != NULL) {
        s_event_cb(connected, s_event_ctx);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            notify_state(false);
            if (s_mode == PLATFORM_WIFI_MODE_STA && s_retry_count < s_retry_limit) {
                esp_wifi_connect();
                s_retry_count++;
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
            }
        } else if (event_id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "SoftAP started");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        notify_state(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t platform_wifi_init(platform_wifi_event_cb_t cb, void *user_ctx)
{
    static bool initialized = false;
    if (initialized) {
        s_event_cb = cb;
        s_event_ctx = user_ctx;
        return ESP_OK;
    }

    s_event_cb = cb;
    s_event_ctx = user_ctx;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_handler_instance),
                        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_ip_handler_instance),
                        TAG, "register IP_EVENT failed");

    initialized = true;
    return ESP_OK;
}

void platform_wifi_build_ap_ssid(char *buffer, size_t buffer_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buffer, buffer_len, "%s-%02X%02X%02X",
             CONFIG_AIRMON_AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
}

static void destroy_netifs(void)
{
    if (s_sta_netif != NULL) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif != NULL) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
}

esp_err_t platform_wifi_prepare_provisioning_sta(void)
{
    platform_wifi_stop();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG, "create sta netif failed");

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_retry_limit = 0;
    s_retry_count = 0;
    s_mode = PLATFORM_WIFI_MODE_STA;
    notify_state(false);
    return ESP_OK;
}

void platform_wifi_stop(void)
{
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    destroy_netifs();
    s_mode = PLATFORM_WIFI_MODE_OFF;
    notify_state(false);
}

esp_err_t platform_wifi_start_ap(const char *ssid)
{
    platform_wifi_stop();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif != NULL, ESP_ERR_NO_MEM, TAG, "create ap netif failed");

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = (uint8_t)strlen(ssid),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
            .pmf_cfg = {.required = false},
        },
    };
    strlcpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "set AP config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start AP failed");

    s_mode = PLATFORM_WIFI_MODE_AP;
    notify_state(false);
    return ESP_OK;
}

esp_err_t platform_wifi_switch_to_ap(const char *ssid)
{
    return platform_wifi_start_ap(ssid);
}

esp_err_t platform_wifi_start_sta(const device_config_t *config, int max_retry, int timeout_ms)
{
    platform_wifi_stop();
    s_retry_limit = max_retry;
    s_retry_count = 0;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG, "create sta netif failed");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, config->wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, config->wifi_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set STA config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start STA failed");

    s_mode = PLATFORM_WIFI_MODE_STA;
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAILED_BIT) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

bool platform_wifi_is_connected(void)
{
    return s_connected;
}

platform_wifi_mode_t platform_wifi_get_mode(void)
{
    return s_mode;
}

int platform_wifi_get_rssi(void)
{
    if (s_mode != PLATFORM_WIFI_MODE_STA || !s_connected) {
        return 0;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

esp_err_t platform_wifi_get_ip(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buffer[0] = '\0';

    if (s_mode != PLATFORM_WIFI_MODE_STA || !s_connected || s_sta_netif == NULL) {
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_sta_netif, &ip_info), TAG, "get ip failed");
    const char *ip_str = ip4addr_ntoa((const ip4_addr_t *)&ip_info.ip);
    strlcpy(buffer, ip_str, buffer_len);
    return ESP_OK;
}
