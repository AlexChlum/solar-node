#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>

#include "wifi_autoconnect.h"
#include "mqtt_client.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));
static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));

static struct led_rgb pixel_buf[4];

/* LED blink thread to isolate I2S timing from other workers. */
static struct k_thread led_thread_data;
K_THREAD_STACK_DEFINE(led_thread_stack, 1024);

static void led_blink_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    bool blink_on = false;
    bool last_connected = false;

    while (1) {
        bool connected = wifi_autoconnect_is_connected();
        if (connected != last_connected) {
            last_connected = connected;
            blink_on = false;
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

        int ret = 0;
        /* Try a few times to recover from transient I/O errors. */
        for (int attempt = 0; attempt < 3; attempt++) {
            ret = led_strip_update_rgb(strip, pixel_buf, 1);
            if (ret == 0) {
                break;
            }
            LOG_ERR("led_strip_update_rgb(blink) failed (attempt %d): %d", attempt + 1, ret);
            k_msleep(50);
        }
        /* If all retries failed the I2S state machine is in ERROR. Use
         * I2S_TRIGGER_PREPARE to reset it back to READY so the next
         * blink attempt can succeed. */
        if (ret != 0) {
            i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
        }

        k_msleep(500);
    }
}

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

    /* Start LED blink thread at slightly higher priority than telemetry workers
     * so LED timing isn't starved by network activity. */
    k_thread_create(&led_thread_data, led_thread_stack,
                    K_THREAD_STACK_SIZEOF(led_thread_stack), led_blink_thread,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);

    /* Main thread becomes an idle poller for WiFi/MQTT startup. */
    bool mqtt_started = false;
    bool last_connected = false;

    while (1) {
        wifi_autoconnect_poll();

        bool connected = wifi_autoconnect_is_connected();
        if (connected != last_connected) {
            last_connected = connected;
            if (connected && !mqtt_started) {
                int rc = solar_mqtt_start();
                if (rc == 0) {
                    mqtt_started = true;
                } else {
                    LOG_WRN("MQTT client failed to start: %d", rc);
                }
            }
        }

        k_msleep(200);
    }

    return 0;
}