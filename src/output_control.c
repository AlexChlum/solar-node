#include "output_control.h"

#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(output_control, LOG_LEVEL_INF);

static const struct device *const pwr3_reg = DEVICE_DT_GET(DT_NODELABEL(pwr3));

int output_control_init(void)
{
	if (!device_is_ready(pwr3_reg)) {
		LOG_ERR("Pwr3 regulator device not ready");
		return -ENODEV;
	}
	LOG_INF("Lights output ready on IO32/Pwr3 (regulator-fixed)");
	return 0;
}

int output_control_set(bool on)
{
	/* Guard against double-enable / double-disable, which would
	 * corrupt the regulator reference count and cause errors.
	 * This also keeps MQTT and shell in sync — either path can
	 * toggle the output and the other sees the correct state. */
	if ((bool)regulator_is_enabled(pwr3_reg) == on) {
		return 0;
	}

	int ret = on ? regulator_enable(pwr3_reg) : regulator_disable(pwr3_reg);
	if (ret < 0) {
		LOG_ERR("Failed to %s Pwr3: %d", on ? "enable" : "disable", ret);
		return ret;
	}

	LOG_INF("Lights output %s", on ? "ON" : "OFF");
	return 0;
}
