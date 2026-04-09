#include "status_led.h"

#include <stdint.h>
#include <string.h>

#include "air_quality.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip_encoder.h"
#include "sensors.h"

#define STATUS_LED_GPIO 48
#define STATUS_LED_RESOLUTION_HZ 10000000
#define STATUS_LED_BRIGHTNESS 24
#define STATUS_LED_UPDATE_MS 500

static const char *TAG = "status_led";

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    status_led_enabled_cb_t enabled_cb;
    void *enabled_user_ctx;
    uint8_t last_pixels[3];
    bool initialized;
} status_led_t;

static status_led_t s_status_led;

static bool status_led_is_enabled(void)
{
    if (s_status_led.enabled_cb == NULL) {
        return true;
    }
    return s_status_led.enabled_cb(s_status_led.enabled_user_ctx);
}

static uint8_t status_led_scale(uint8_t value)
{
    return (uint8_t)((value * STATUS_LED_BRIGHTNESS) / 255U);
}

static esp_err_t status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_status_led.initialized) {
        return ESP_OK;
    }

    /* WS2812B-compatible LED with GBR byte order (verify for your LED variant). */
    uint8_t pixels[3] = {
        status_led_scale(g),
        status_led_scale(b),
        status_led_scale(r),
    };
    if (memcmp(s_status_led.last_pixels, pixels, sizeof(pixels)) == 0) {
        return ESP_OK;
    }

    memcpy(s_status_led.last_pixels, pixels, sizeof(pixels));
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_transmit(s_status_led.channel,
                                     s_status_led.encoder,
                                     pixels,
                                     sizeof(pixels),
                                     &tx_config),
                        TAG, "status led transmit failed");
    return rmt_tx_wait_all_done(s_status_led.channel, 100);
}

static esp_err_t status_led_init_hardware(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = STATUS_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = STATUS_LED_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan_config, &s_status_led.channel),
                        TAG, "status led channel init failed");

    led_strip_encoder_config_t encoder_config = {
        .resolution = STATUS_LED_RESOLUTION_HZ,
    };
    esp_err_t err = rmt_new_led_strip_encoder(&encoder_config, &s_status_led.encoder);
    if (err != ESP_OK) {
        rmt_del_channel(s_status_led.channel);
        memset(&s_status_led, 0, sizeof(s_status_led));
        return err;
    }

    err = rmt_enable(s_status_led.channel);
    if (err != ESP_OK) {
        rmt_del_encoder(s_status_led.encoder);
        rmt_del_channel(s_status_led.channel);
        memset(&s_status_led, 0, sizeof(s_status_led));
        return err;
    }

    s_status_led.initialized = true;
    return status_led_set_rgb(0, 0, 0);
}

esp_err_t status_led_update_now(void)
{
    if (!status_led_is_enabled()) {
        return status_led_set_rgb(0, 0, 0);
    }

    sensor_snapshot_t snapshot = {0};
    if (sensors_get_snapshot(&snapshot) != ESP_OK) {
        return ESP_OK;
    }

    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    air_quality_assessment_t assessment = {0};
    air_quality_compute_overall_assessment(&snapshot, &assessment);
    if (assessment.valid) {
        switch (assessment.category) {
        case AIR_QUALITY_CATEGORY_GOOD:
            green = 255;
            break;
        case AIR_QUALITY_CATEGORY_MODERATE:
            red = 255;
            green = 180;
            break;
        case AIR_QUALITY_CATEGORY_UNHEALTHY_SENSITIVE:
            red = 255;
            green = 80;
            break;
        case AIR_QUALITY_CATEGORY_UNHEALTHY:
            red = 255;
            break;
        case AIR_QUALITY_CATEGORY_VERY_UNHEALTHY:
            red = 160;
            blue = 255;
            break;
        case AIR_QUALITY_CATEGORY_HAZARDOUS:
            red = 255;
            blue = 120;
            break;
        case AIR_QUALITY_CATEGORY_UNKNOWN:
        default:
            blue = 255;
            break;
        }
        return status_led_set_rgb(red, green, blue);
    }

    bool blink_on = ((esp_timer_get_time() / 1000 / STATUS_LED_UPDATE_MS) % 2) == 0;
    if (sensors_any_ready()) {
        blue = blink_on ? 255 : 0;
    } else {
        red = blink_on ? 255 : 0;
        blue = blink_on ? 96 : 0;
    }
    return status_led_set_rgb(red, green, blue);
}

static void status_led_task(void *arg)
{
    (void)arg;
    while (true) {
        status_led_update_now();
        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_UPDATE_MS));
    }
}

esp_err_t status_led_start(status_led_enabled_cb_t enabled_cb, void *user_ctx)
{
    memset(&s_status_led, 0, sizeof(s_status_led));
    s_status_led.enabled_cb = enabled_cb;
    s_status_led.enabled_user_ctx = user_ctx;

    ESP_RETURN_ON_ERROR(status_led_init_hardware(), TAG, "status led hardware init failed");
    if (xTaskCreate(status_led_task, "status_led_task", 4096, NULL, 3, NULL) != pdPASS) {
        status_led_set_rgb(0, 0, 0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool status_led_is_ready(void)
{
    return s_status_led.initialized;
}
