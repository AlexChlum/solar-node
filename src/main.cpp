#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct device *strip = DEVICE_DT_GET(DT_ALIAS(led_strip));

int main(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip device is not ready");
        return 0;
    }

    LOG_INF("LED strip ready; cycling RGB");

    struct led_rgb red = {.r = 0xFF, .g = 0x00, .b = 0x00};
    struct led_rgb green = {.r = 0x00, .g = 0xFF, .b = 0x00};
    struct led_rgb blue = {.r = 0x00, .g = 0x00, .b = 0xFF};

    while (1) {
        int ret = led_strip_update_rgb(strip, &red, 1);
        if (ret) {
            LOG_ERR("led_strip_update_rgb(red) failed: %d", ret);
        }
        k_msleep(1000);
        ret = led_strip_update_rgb(strip, &green, 1);
        if (ret) {
            LOG_ERR("led_strip_update_rgb(green) failed: %d", ret);
        }
        k_msleep(1000);
        ret = led_strip_update_rgb(strip, &blue, 1);
        if (ret) {
            LOG_ERR("led_strip_update_rgb(blue) failed: %d", ret);
        }
        k_msleep(1000);
    }

    return 0;
}