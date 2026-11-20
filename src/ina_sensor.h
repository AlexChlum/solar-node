#ifndef SRC_INA_SENSOR_H_
#define SRC_INA_SENSOR_H_

#include <stdint.h>

/* Returns 0 on success, negative errno on failure. */
int ina_sensor_init(void);
int ina_sensor_get_averages(float *battery_v, float *battery_current_a,
			    float *battery_power_w, uint32_t *sample_count);

#endif /* SRC_INA_SENSOR_H_ */
