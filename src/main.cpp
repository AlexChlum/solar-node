#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct device *strip = DEVICE_DT_GET(DT_ALIAS(led_strip));

static bool wifi_is_connected(void) {
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        return false;
    }

    return net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != nullptr;
}

int main(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip device is not ready");
        return 0;
    }

    LOG_INF("=== Solar Node Starting ===");
    LOG_INF("LED strip ready; WiFi is shell-driven in this build");
    LOG_INF("Use shell commands:");
    LOG_INF("  wifi scan");
    LOG_INF("  wifi connect -s \"<SSID>\" -p \"<PASS>\" -k 1");
    LOG_INF("  wifi status");

    LOG_INF("LED behavior: blinking GREEN until WiFi connects, then BLUE");

    // 50% brightness colors
    struct led_rgb off = {.r = 0x00, .g = 0x00, .b = 0x00};
    struct led_rgb green_half = {.r = 0x00, .g = 0x80, .b = 0x00};
    struct led_rgb blue_half = {.r = 0x00, .g = 0x00, .b = 0x80};
    bool last_connected = false;

    while (1) {
        bool connected = wifi_is_connected();
        if (connected != last_connected) {
            LOG_INF("WiFi state changed: %s", connected ? "connected" : "disconnected");
            last_connected = connected;
        }

        struct led_rgb on = connected ? blue_half : green_half;

        int ret = led_strip_update_rgb(strip, &on, 1);
        if (ret) {
            LOG_ERR("led_strip_update_rgb(on) failed: %d", ret);
        }
        k_msleep(400);

        ret = led_strip_update_rgb(strip, &off, 1);
        if (ret) {
            LOG_ERR("led_strip_update_rgb(off) failed: %d", ret);
        }
        k_msleep(400);
    }

    return 0;
}