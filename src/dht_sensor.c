#include "dht_sensor.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dht_sensor, LOG_LEVEL_INF);

static const struct device *const dht22_dev = DEVICE_DT_GET(DT_NODELABEL(dht22));

int dht_sensor_init(void)
{
	if (!device_is_ready(dht22_dev)) {
		LOG_ERR("DHT22 device not ready");
		return -ENODEV;
	}
	LOG_INF("DHT22 ready on IO26");
	return 0;
}

int dht_sensor_read(float *temp_c, float *humidity_pct)
{
	int rc = sensor_sample_fetch(dht22_dev);
	if (rc < 0) {
		LOG_WRN("DHT22 sample fetch failed: %d", rc);
		return rc;
	}

	struct sensor_value temp, humidity;

	rc = sensor_channel_get(dht22_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (rc < 0) {
		LOG_WRN("DHT22 temp channel read failed: %d", rc);
		return rc;
	}

	rc = sensor_channel_get(dht22_dev, SENSOR_CHAN_HUMIDITY, &humidity);
	if (rc < 0) {
		LOG_WRN("DHT22 humidity channel read failed: %d", rc);
		return rc;
	}

	/* sensor_value: val1 = integer part, val2 = micro (1e-6) fractional part */
	*temp_c       = (float)temp.val1     + (float)temp.val2     / 1000000.0f;
	*humidity_pct = (float)humidity.val1 + (float)humidity.val2 / 1000000.0f;
	return 0;
}
