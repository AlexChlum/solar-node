/* MQTT client interface for Solar Node */
#ifndef SRC_MQTT_CLIENT_H_
#define SRC_MQTT_CLIENT_H_

#ifdef __cplusplus
extern "C" {
#endif

void solar_mqtt_init(void);
int solar_mqtt_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SRC_MQTT_CLIENT_H_ */
