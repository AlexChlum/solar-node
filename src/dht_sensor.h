#ifndef SRC_DHT_SENSOR_H_
#define SRC_DHT_SENSOR_H_

/* Returns 0 on success, negative errno on failure. */
int dht_sensor_init(void);
int dht_sensor_read(float *temp_c, float *humidity_pct);

#endif /* SRC_DHT_SENSOR_H_ */
