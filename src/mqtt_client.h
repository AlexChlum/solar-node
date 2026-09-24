/* Minimal MQTT client interface for Solar Node
 * This is a lightweight stub that will be expanded to use Zephyr's MQTT
 * library. For now it provides startup and a test publish entrypoint.
 */
#ifndef SRC_MQTT_CLIENT_H_
#define SRC_MQTT_CLIENT_H_

/* Minimal header - avoid pulling Zephyr-specific headers here to keep compile
 * ordering simple. Implementation may include Zephyr headers as needed. */

#ifdef __cplusplus
extern "C" {
#endif

void solar_mqtt_init(void);
int solar_mqtt_start(void);
int solar_mqtt_publish_test(const char *topic, const char *payload);

#ifdef __cplusplus
}
#endif

#endif /* SRC_MQTT_CLIENT_H_ */
