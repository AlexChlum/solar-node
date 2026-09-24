#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "wifi_autoconnect.h"
#include "mqtt_client.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));

static struct led_rgb pixel_buf[4];

int main(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip device is not ready");
        return 0;
    }

    LOG_INF("=== Solar Node Starting ===");
    LOG_INF("LED strip ready; WiFi auto-connect is enabled");
    LOG_INF("Shell is still available for diagnostics:");
    LOG_INF("  wifi status");
    LOG_INF("  net iface");

    int auto_connect_ret = wifi_autoconnect_start();
    if (auto_connect_ret) {
        LOG_WRN("Auto-connect request failed: %d", auto_connect_ret);
    }

    /* Initialize the Solar MQTT helper (stub implementation for now). */
    solar_mqtt_init();

    LOG_INF("LED behavior: blinking GREEN until WiFi connects, then BLUE");

    bool blink_on = false;
    bool last_connected = false;
    bool mqtt_started = false;

    while (1) {
        wifi_autoconnect_poll();

        bool connected = wifi_autoconnect_is_connected();
        if (connected != last_connected) {
            last_connected = connected;
            blink_on = false;
            if (connected && !mqtt_started) {
                int rc = solar_mqtt_start();
                if (rc == 0) {
                    mqtt_started = true;
                } else {
                    LOG_WRN("MQTT client failed to start: %d", rc);
                }
            }
        }

        blink_on = !blink_on;
        if (blink_on) {
            if (connected) {
                pixel_buf[0].r = 0x00;
                pixel_buf[0].g = 0x00;
                pixel_buf[0].b = 0x80;
            } else {
                pixel_buf[0].r = 0x00;
                pixel_buf[0].g = 0x80;
                pixel_buf[0].b = 0x00;
            }
        } else {
            pixel_buf[0].r = 0x00;
            pixel_buf[0].g = 0x00;
            pixel_buf[0].b = 0x00;
        }

        int ret = led_strip_update_rgb(strip, pixel_buf, 1);
        if (ret) {
            LOG_ERR("led_strip_update_rgb(blink) failed: %d", ret);
        }

        k_msleep(500);
    }

    return 0;
}