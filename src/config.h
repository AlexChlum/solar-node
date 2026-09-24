#ifndef SRC_CONFIG_H_
#define SRC_CONFIG_H_

// WiFi Configuration
#define WIFI_SSID "ChlumNet"
#define WIFI_PASSWORD "W!f!P@ssword"

// MQTT Configuration
#define MQTT_BROKER_HOST "192.168.1.182"
#define MQTT_BROKER_PORT 1883
// Optional MQTT auth (leave empty to disable)
#define MQTT_USERNAME "admin"
#define MQTT_PASSWORD "deutschland"

// MQTT client identifier and Home Assistant prefix
#define MQTT_CLIENT_ID "solar_node"
#define MQTT_DISCOVERY_PREFIX "homeassistant"

// Device Configuration
#define DEVICE_ID "solar_node"
#define DEVICE_NAME "Solar Node"

#endif /* SRC_CONFIG_H_ */
