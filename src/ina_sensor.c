#include "ina_sensor.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/devicetree.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

LOG_MODULE_REGISTER(ina_sensor, LOG_LEVEL_INF);

#define INA_SAMPLE_PERIOD_MS 1000

static const struct device *const dev_battery_ina = DEVICE_DT_GET(DT_NODELABEL(ina260));

static struct k_thread ina_thread_data;
K_THREAD_STACK_DEFINE(ina_thread_stack, 2048);

static struct k_mutex avg_lock;
static double sum_battery_v;
static double sum_battery_current_a;
static double sum_battery_power_w;
static uint32_t sample_count;
static bool started;

static float sensor_value_to_f32(const struct sensor_value *val)
{
	return (float)val->val1 + (float)val->val2 / 1000000.0f;
}

static int read_battery_ina260(float *battery_v, float *battery_current_a, float *battery_power_w)
{
	int ret = sensor_sample_fetch(dev_battery_ina);
	if (ret < 0) {
		LOG_WRN("sensor_sample_fetch(%s) failed: %d", dev_battery_ina->name, ret);
		return ret;
	}

	struct sensor_value voltage_val;
	struct sensor_value current_val;
	struct sensor_value power_val;

	ret = sensor_channel_get(dev_battery_ina, SENSOR_CHAN_VOLTAGE, &voltage_val);
	if (ret < 0) {
		LOG_WRN("sensor_channel_get(%s, VOLTAGE) failed: %d", dev_battery_ina->name, ret);
		return ret;
	}

	ret = sensor_channel_get(dev_battery_ina, SENSOR_CHAN_CURRENT, &current_val);
	if (ret < 0) {
		LOG_WRN("sensor_channel_get(%s, CURRENT) failed: %d", dev_battery_ina->name, ret);
		return ret;
	}

	ret = sensor_channel_get(dev_battery_ina, SENSOR_CHAN_POWER, &power_val);
	if (ret < 0) {
		LOG_WRN("sensor_channel_get(%s, POWER) failed: %d", dev_battery_ina->name, ret);
		return ret;
	}

	*battery_v = sensor_value_to_f32(&voltage_val);
	*battery_current_a = sensor_value_to_f32(&current_val);
	*battery_power_w = sensor_value_to_f32(&power_val);

	return 0;
}

static void ina_sensor_worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		float battery_v;
		float battery_current_a;
		float battery_power_w;

		int rc = read_battery_ina260(&battery_v, &battery_current_a, &battery_power_w);
		if (rc == 0) {
			k_mutex_lock(&avg_lock, K_FOREVER);
			sum_battery_v += battery_v;
			sum_battery_current_a += battery_current_a;
			sum_battery_power_w += battery_power_w;
			sample_count++;
			k_mutex_unlock(&avg_lock);
		}

		k_msleep(INA_SAMPLE_PERIOD_MS);
	}
}

int ina_sensor_init(void)
{
	if (started) {
		return 0;
	}

	if (!device_is_ready(dev_battery_ina)) {
		LOG_ERR("Battery INA260 %s not ready", dev_battery_ina->name);
		return -ENODEV;
	}

	k_mutex_init(&avg_lock);
	sum_battery_v = 0.0;
	sum_battery_current_a = 0.0;
	sum_battery_power_w = 0.0;
	sample_count = 0U;

	k_thread_create(&ina_thread_data, ina_thread_stack,
			K_THREAD_STACK_SIZEOF(ina_thread_stack), ina_sensor_worker,
			NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	started = true;
	LOG_INF("INA sensor thread started (period=%d ms)", INA_SAMPLE_PERIOD_MS);

	return 0;
}

int ina_sensor_get_averages(float *battery_v, float *battery_current_a,
			    float *battery_power_w, uint32_t *samples)
{
	if (!started) {
		return -EACCES;
	}

	if (!battery_v || !battery_current_a || !battery_power_w || !samples) {
		return -EINVAL;
	}

	k_mutex_lock(&avg_lock, K_FOREVER);

	if (sample_count == 0U) {
		k_mutex_unlock(&avg_lock);
		return -EAGAIN;
	}

	*battery_v = (float)(sum_battery_v / (double)sample_count);
	*battery_current_a = (float)(sum_battery_current_a / (double)sample_count);
	*battery_power_w = (float)(sum_battery_power_w / (double)sample_count);
	*samples = sample_count;

	sum_battery_v = 0.0;
	sum_battery_current_a = 0.0;
	sum_battery_power_w = 0.0;
	sample_count = 0U;

	k_mutex_unlock(&avg_lock);
	return 0;
}
