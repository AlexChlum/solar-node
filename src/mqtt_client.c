/* Full MQTT client: plain (non-TLS) connection with RSSI, battery INA260
 * voltage/current/power, Battery SoC, Temperature, and Humidity telemetry.
 * - resolves MQTT_BROKER_HOST
 * - connects to broker, sets LWT to solar_node/availability (offline retained)
 * - reads battery bus voltage, current, and power via INA260 sensor node
 * - computes battery SoC from AGM discharge curve using INA260 voltage
 * - reads DHT22 on IO26 for temperature and humidity
 */

#include "mqtt_client.h"
#include "config.h"
#include "wifi_autoconnect.h"
#include "dht_sensor.h"
#include "output_control.h"
#include "battery_soc.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/devicetree.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(solar_mqtt, LOG_LEVEL_INF);

/* Battery INA260 device from DTS */
static const struct device *const dev_battery_ina = DEVICE_DT_GET(DT_NODELABEL(ina260));

/* AGM battery SoC thresholds */
#define BATT_FULL_VOLTAGE_V   12.70f  /* resting voltage at 100 % SoC */

/* Buffers and client instance */
static struct mqtt_client client;
static uint8_t rx_buffer[512];
static uint8_t tx_buffer[512];

/* WiFi scan helpers */
static struct net_mgmt_event_callback scan_cb;
static struct k_sem scan_sem;
static int8_t last_rssi = INT8_MIN;

/* Worker thread that performs periodic scans, reads sensor values, and publishes telemetry */
static struct k_thread mqtt_thread_data;
K_THREAD_STACK_DEFINE(mqtt_thread_stack, 4096);

/* Separate MQTT I/O thread to process CONNACK, PINGRESP and disconnects */
static struct k_thread mqtt_io_thread_data;
K_THREAD_STACK_DEFINE(mqtt_io_thread_stack, 3072);

static bool started;
static volatile bool mqtt_connected = false;

static int mqtt_do_connect(void);

static float sensor_value_to_f32(const struct sensor_value *val)
{
	return (float)val->val1 + (float)val->val2 / 1000000.0f;
}

static int read_battery_ina260(float *out_battery_v, float *out_battery_current_a, float *out_battery_power_w)
{
	if (!device_is_ready(dev_battery_ina)) {
		LOG_ERR("Battery INA260 %s not ready", dev_battery_ina->name);
		return -ENODEV;
	}

	int ret = sensor_sample_fetch(dev_battery_ina);
	if (ret < 0) {
		LOG_ERR("sensor_sample_fetch(%s) failed: %d", dev_battery_ina->name, ret);
		return ret;
	}

	struct sensor_value voltage_val;
	struct sensor_value current_val;
	struct sensor_value power_val;

	ret = sensor_channel_get(dev_battery_ina, SENSOR_CHAN_VOLTAGE, &voltage_val);
	if (ret < 0) {
		LOG_ERR("sensor_channel_get(%s, VOLTAGE) failed: %d", dev_battery_ina->name, ret);
		return ret;
	}

	ret = sensor_channel_get(dev_battery_ina, SENSOR_CHAN_CURRENT, &current_val);
	if (ret < 0) {
		LOG_ERR("sensor_channel_get(%s, CURRENT) failed: %d", dev_battery_ina->name, ret);
		return ret;
	}

	ret = sensor_channel_get(dev_battery_ina, SENSOR_CHAN_POWER, &power_val);
	if (ret < 0) {
		LOG_ERR("sensor_channel_get(%s, POWER) failed: %d", dev_battery_ina->name, ret);
		return ret;
	}

	*out_battery_v = sensor_value_to_f32(&voltage_val);
	*out_battery_current_a = sensor_value_to_f32(&current_val);
	*out_battery_power_w = sensor_value_to_f32(&power_val);

	LOG_INF("Battery INA260 -> V=%.6fV I=%.6fA P=%.6fW",
		*out_battery_v, *out_battery_current_a, *out_battery_power_w);

	return 0;
}

static int mqtt_socket_fd(const struct mqtt_client *c)
{
	if (c->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		return c->transport.tcp.sock;
	}
#if defined(CONFIG_MQTT_LIB_TLS)
	if (c->transport.type == MQTT_TRANSPORT_SECURE) {
		return c->transport.tls.sock;
	}
#endif
	return -1;
}

static void mqtt_io_worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		int fd = mqtt_socket_fd(&client);
		if (fd < 0) {
			if (wifi_autoconnect_is_connected() && started && !mqtt_connected) {
				/* WiFi is up but socket is gone — reconnect */
				k_msleep(5000);
				LOG_INF("MQTT reconnecting...");
				mqtt_do_connect();
			} else {
				k_msleep(250);
			}
			continue;
		}

		int timeout = mqtt_keepalive_time_left(&client);
		if (timeout < 0 || timeout > 1000) {
			timeout = 1000;
		}

		struct zsock_pollfd pfd = {
			.fd = fd,
			.events = ZSOCK_POLLIN,
			.revents = 0,
		};

		int prc = zsock_poll(&pfd, 1, timeout);
		if (prc < 0) {
			LOG_WRN("mqtt poll failed: %d", errno);
			k_msleep(250);
			continue;
		}

		if (prc > 0 && (pfd.revents & ZSOCK_POLLIN)) {
			int irc = mqtt_input(&client);
			if (irc && irc != -EAGAIN) {
				LOG_WRN("mqtt_input failed: %d", irc);
			}
		}

		if (prc > 0 && (pfd.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
			LOG_WRN("mqtt socket event: revents=0x%x", pfd.revents);
			mqtt_connected = false;
		}

		int lrc = mqtt_live(&client);
		if (lrc && lrc != -EAGAIN) {
			LOG_WRN("mqtt_live failed: %d", lrc);
		}
	}
}

static void mqtt_publish_str(const char *topic_str, const char *payload, bool retain)
{
	struct mqtt_publish_param param;
	memset(&param, 0, sizeof(param));

	struct mqtt_utf8 topic = {
		.utf8 = (const uint8_t *)topic_str,
		.size = strlen(topic_str)
	};

	param.message.topic.topic = topic;
	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = strlen(payload);
	param.message_id = (uint16_t)(k_cycle_get_32() & 0xffff);
	param.retain_flag = retain ? 1 : 0;

	int rc = mqtt_publish(&client, &param);
	if (rc) {
		LOG_WRN("mqtt_publish to %s failed: %d", topic_str, rc);
	} else {
		LOG_INF("Published -> %s: %s", topic_str, payload);
	}
}

static void publish_telemetry(float battery_v, float battery_current_a, float battery_power_w,
			      int soc,
			      bool dht_ok, float temp_c, float humidity_pct)
{
	char payload[32];

	/* 1. RSSI */
	if (last_rssi != INT8_MIN) {
		snprintf(payload, sizeof(payload), "%d", last_rssi);
		mqtt_publish_str("solar_node/sensor/rssi/state", payload, false);
	}

	/* 2. Battery Voltage */
	snprintf(payload, sizeof(payload), "%.2f", battery_v);
	mqtt_publish_str("solar_node/sensor/battery_voltage/state", payload, false);

	/* 3. Battery Current */
	snprintf(payload, sizeof(payload), "%.3f", battery_current_a);
	mqtt_publish_str("solar_node/sensor/battery_current/state", payload, false);

	/* 4. Battery Power */
	snprintf(payload, sizeof(payload), "%.3f", battery_power_w);
	mqtt_publish_str("solar_node/sensor/battery_power/state", payload, false);

	/* 5. Battery SoC */
	snprintf(payload, sizeof(payload), "%d", soc);
	mqtt_publish_str("solar_node/sensor/battery_soc/state", payload, false);

	/* 6 & 7. Temperature and Humidity (skipped if DHT22 read failed) */
	if (dht_ok) {
		snprintf(payload, sizeof(payload), "%.1f", temp_c);
		mqtt_publish_str("solar_node/sensor/temperature/state", payload, false);

		snprintf(payload, sizeof(payload), "%.1f", humidity_pct);
		mqtt_publish_str("solar_node/sensor/humidity/state", payload, false);
	}
}

static void mqtt_event_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	LOG_INF("mqtt_event_handler: type=%d result=%d", evt->type, evt->result);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0 && evt->param.connack.return_code == MQTT_CONNECTION_ACCEPTED) {
			LOG_INF("MQTT connected (CONNACK OK)");
			mqtt_connected = true;

			/* Availability: online (retained) */
			mqtt_publish_str("solar_node/availability", "online", true);

			/* Subscribe to the switch command topic */
			static struct mqtt_topic lights_cmd_topic = {
				.topic = MQTT_UTF8_LITERAL("solar_node/switch/lights/command"),
				.qos = MQTT_QOS_0_AT_MOST_ONCE,
			};
			struct mqtt_subscription_list lights_sub = {
				.list = &lights_cmd_topic,
				.list_count = 1,
				.message_id = 1,
			};
			int sub_rc = mqtt_subscribe(&client, &lights_sub);
			if (sub_rc) {
				LOG_WRN("mqtt_subscribe(lights command) failed: %d", sub_rc);
			} else {
				LOG_INF("Subscribed to solar_node/switch/lights/command");
			}

			/* Home Assistant Discovery */
			static const char disc_rssi[] =
				"{\"name\":\"RSSI\","
				"\"state_topic\":\"solar_node/sensor/rssi/state\","
				"\"unit_of_measurement\":\"dBm\","
				"\"device_class\":\"signal_strength\","
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_rssi\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			static const char disc_battery[] =
				"{\"name\":\"Battery Voltage\","
				"\"state_topic\":\"solar_node/sensor/battery_voltage/state\","
				"\"unit_of_measurement\":\"V\","
				"\"device_class\":\"voltage\","
				"\"suggested_display_precision\":2,"
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_battery_v\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			static const char disc_battery_current[] =
				"{\"name\":\"Battery Current\","
				"\"state_topic\":\"solar_node/sensor/battery_current/state\","
				"\"unit_of_measurement\":\"A\","
				"\"device_class\":\"current\","
				"\"suggested_display_precision\":3,"
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_battery_current\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			static const char disc_battery_power[] =
				"{\"name\":\"Battery Power\","
				"\"state_topic\":\"solar_node/sensor/battery_power/state\","
				"\"unit_of_measurement\":\"W\","
				"\"device_class\":\"power\","
				"\"suggested_display_precision\":3,"
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_battery_power\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			static const char disc_temp[] =
				"{\"name\":\"Temperature\","
				"\"state_topic\":\"solar_node/sensor/temperature/state\","
				"\"unit_of_measurement\":\"°C\","
				"\"device_class\":\"temperature\","
				"\"suggested_display_precision\":1,"
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_temp\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			static const char disc_humidity[] =
				"{\"name\":\"Humidity\","
				"\"state_topic\":\"solar_node/sensor/humidity/state\","
				"\"unit_of_measurement\":\"%\","
				"\"device_class\":\"humidity\","
				"\"suggested_display_precision\":1,"
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_humidity\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			static const char disc_lights[] =
				"{\"name\":\"Lights\","
				"\"command_topic\":\"solar_node/switch/lights/command\","
				"\"state_topic\":\"solar_node/switch/lights/state\","
				"\"payload_on\":\"ON\","
				"\"payload_off\":\"OFF\","
				"\"retain\":true,"
				"\"unique_id\":\"solar_node_lights\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			static const char disc_battery_soc[] =
				"{\"name\":\"Battery SoC\","
				"\"state_topic\":\"solar_node/sensor/battery_soc/state\","
				"\"unit_of_measurement\":\"%\","
				"\"device_class\":\"battery\","
				"\"suggested_display_precision\":0,"
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_battery_soc\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			/* Yield between each discovery publish — rapid back-to-back sends
			 * exhaust the TCP TX buffers and cause the broker to drop the connection. */
			/* Clear old retained solar voltage discovery because solar ADC telemetry was removed. */
			mqtt_publish_str("homeassistant/sensor/solar_node/solar_voltage/config", "", true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/rssi/config", disc_rssi, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/battery_voltage/config", disc_battery, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/battery_current/config", disc_battery_current, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/battery_power/config", disc_battery_power, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/temperature/config", disc_temp, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/humidity/config", disc_humidity, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/switch/solar_node/lights/config", disc_lights, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/battery_soc/config", disc_battery_soc, true);
			mqtt_publish_str("solar_node/switch/lights/state", "OFF", true);

		} else {
			LOG_WRN("MQTT CONNACK failed (%d) rc=%d", evt->result, evt->param.connack.return_code);
		}
		break;
	case MQTT_EVT_PUBLISH:
		{
			/* In Zephyr MQTT, payload.data is always NULL on receive.
			 * The bytes live in the TCP socket buffer and must be read
			 * with mqtt_read_publish_payload_blocking(). Drain ALL bytes
			 * regardless of topic to keep the stream in sync. */
			size_t total_len = evt->param.publish.message.payload.len;
			char payload_buf[16];
			size_t read_len = MIN(total_len, sizeof(payload_buf) - 1);

			int bytes_read = mqtt_read_publish_payload_blocking(
				&client, payload_buf, read_len);
			if (bytes_read < 0) {
				LOG_WRN("mqtt_read_publish_payload failed: %d", bytes_read);
				break;
			}
			payload_buf[bytes_read] = '\0';

			/* Drain any bytes beyond our buffer to keep the stream in sync */
			size_t remaining = total_len - (size_t)bytes_read;
			while (remaining > 0) {
				char drain[32];
				size_t chunk = MIN(remaining, sizeof(drain));
				int rc = mqtt_read_publish_payload_blocking(&client, drain, chunk);
				if (rc <= 0) {
					break;
				}
				remaining -= (size_t)rc;
			}

			const struct mqtt_topic *topic = &evt->param.publish.message.topic;
			if (topic->topic.size == strlen("solar_node/switch/lights/command") &&
			    memcmp(topic->topic.utf8, "solar_node/switch/lights/command",
				   topic->topic.size) == 0) {
				bool light_on;
				if (strcmp(payload_buf, "ON") == 0) {
					light_on = true;
				} else if (strcmp(payload_buf, "OFF") == 0) {
					light_on = false;
				} else {
					LOG_WRN("Unhandled lights payload: %s", payload_buf);
					break;
				}

				int rc = output_control_set(light_on);
				if (rc == 0) {
					mqtt_publish_str("solar_node/switch/lights/state",
							 light_on ? "ON" : "OFF", true);
				}
			}
		}
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_WRN("MQTT disconnected: %d", evt->result);
		mqtt_connected = false;
		break;
	default:
		break;
	}
}

static void scan_event_cb(struct net_mgmt_event_callback *cb,
			  uint64_t mgmt_event,
			  struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_WIFI_SCAN_RESULT) {
		if (!cb->info) {
			return;
		}
		const struct wifi_scan_result *res = (const struct wifi_scan_result *)cb->info;
		if (res->ssid_length && (res->ssid_length == strlen(WIFI_SSID)) &&
		    (memcmp(res->ssid, WIFI_SSID, res->ssid_length) == 0)) {
			last_rssi = res->rssi;
			LOG_INF("scan_event_cb: Found SSID '%s' RSSI=%d", WIFI_SSID, last_rssi);
		} else {
			LOG_DBG("scan_event_cb: scan result for SSID len=%d", res->ssid_length);
		}
	} else if (mgmt_event == NET_EVENT_WIFI_SCAN_DONE) {
		LOG_INF("scan_event_cb: scan done");
		k_sem_give(&scan_sem);
	}
}

static void mqtt_worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/* Wait until connected to WiFi and MQTT broker */
		if (!wifi_autoconnect_is_connected() || !mqtt_connected) {
			k_msleep(2000);
			continue;
		}

		/* Trigger WiFi scan for RSSI */
		struct wifi_scan_params params = { 0 };
		params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
		params.max_bss_cnt = 20;

		struct net_if *iface = net_if_get_default();
		if (!iface) {
			k_msleep(5000);
			continue;
		}

		last_rssi = INT8_MIN;
		int rc = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params, sizeof(params));
		if (rc) {
			LOG_WRN("Failed to request WiFi scan: %d", rc);
		} else {
			/* Wait for scan done or timeout */
			if (k_sem_take(&scan_sem, K_SECONDS(10)) != 0) {
				LOG_WRN("WiFi scan timed out");
			}
			/* ESP32 WiFi ISRs run above RTOS IRQ lock level; wait for them to settle
			 * before the DHT22 1-wire read or timing errors cause EIO. */
			k_msleep(500);
		}

		/* Read battery INA260 measurements */
		float battery_v = 0.0f;
		float battery_current_a = 0.0f;
		float battery_power_w = 0.0f;

		int err = read_battery_ina260(&battery_v, &battery_current_a, &battery_power_w);
		if (err) {
			LOG_ERR("Failed to read battery INA260: %d", err);
		}

		/* Compute battery SoC from INA260 bus voltage. */
		int soc = (battery_v >= BATT_FULL_VOLTAGE_V) ? 100 : battery_soc_from_voltage(battery_v);

		/* Read DHT22 Temperature & Humidity */
		float temp_c = 0.0f;
		float humidity_pct = 0.0f;
		bool dht_ok = (dht_sensor_read(&temp_c, &humidity_pct) == 0);

		/* Publish all gathered telemetry */
		publish_telemetry(battery_v, battery_current_a, battery_power_w, soc,
				  dht_ok, temp_c, humidity_pct);

		/* Publish cycle every 60 seconds */
		k_msleep(60000);
	}
}

void solar_mqtt_init(void)
{
	if (started) {
		return;
	}

	mqtt_client_init(&client);
	client.evt_cb = mqtt_event_handler;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);
	client.clean_session = 1;
	client.keepalive = 60;
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	/* LWT */
	static struct mqtt_utf8 will_msg = MQTT_UTF8_LITERAL("offline");
	static struct mqtt_utf8 will_topic = MQTT_UTF8_LITERAL("solar_node/availability");
	static struct mqtt_topic will_t;
	will_t.topic = will_topic;
	will_t.qos = MQTT_QOS_0_AT_MOST_ONCE;
	client.will_topic = &will_t;
	client.will_message = &will_msg;
	client.will_retain = 1;

	k_sem_init(&scan_sem, 0, 1);

	if (!device_is_ready(dev_battery_ina)) {
		LOG_ERR("battery INA260 sensor not ready");
	}

	dht_sensor_init();
	if (output_control_init() < 0) {
		LOG_WRN("Output control init failed; switch will remain unavailable");
	}

	/* Register scan callbacks */
	net_mgmt_init_event_callback(&scan_cb, scan_event_cb,
				     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&scan_cb);

	started = true;
}

/* Resolves broker address and opens the MQTT TCP connection. Called at startup
 * and by mqtt_io_worker on reconnect; does not spawn threads. */
static int mqtt_do_connect(void)
{
	LOG_INF("Connecting to MQTT broker %s:%d", MQTT_BROKER_HOST, MQTT_BROKER_PORT);

	struct zsock_addrinfo *res = NULL;
	char service[8];
	snprintf(service, sizeof(service), "%d", MQTT_BROKER_PORT);

	int gai = zsock_getaddrinfo(MQTT_BROKER_HOST, service, NULL, &res);
	if (gai == 0 && res) {
		struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
		static struct sockaddr_in broker_addr_dns;
		broker_addr_dns = *addr;
		broker_addr_dns.sin_port = htons(MQTT_BROKER_PORT);
		client.broker = (struct sockaddr *)&broker_addr_dns;
	} else {
		/* Fallback: parse MQTT_BROKER_HOST as a numeric IPv4 address */
		static struct sockaddr_in broker_addr_static;
		memset(&broker_addr_static, 0, sizeof(broker_addr_static));
		int p = zsock_inet_pton(AF_INET, MQTT_BROKER_HOST,
					&broker_addr_static.sin_addr);
		if (p == 1) {
			broker_addr_static.sin_family = AF_INET;
			broker_addr_static.sin_port = htons(MQTT_BROKER_PORT);
			client.broker = (struct sockaddr *)&broker_addr_static;
		} else {
			LOG_ERR("DNS resolution failed for %s: %d", MQTT_BROKER_HOST, gai);
			zsock_freeaddrinfo(res);
			return -ENOTCONN;
		}
	}

	client.client_id = MQTT_UTF8_LITERAL(MQTT_CLIENT_ID);

	static struct mqtt_utf8 username_utf8;
	static struct mqtt_utf8 password_utf8;
	if (strlen(MQTT_USERNAME) > 0) {
		username_utf8.utf8 = (const uint8_t *)MQTT_USERNAME;
		username_utf8.size = strlen(MQTT_USERNAME);
		client.user_name = &username_utf8;
	}
	if (strlen(MQTT_PASSWORD) > 0) {
		password_utf8.utf8 = (const uint8_t *)MQTT_PASSWORD;
		password_utf8.size = strlen(MQTT_PASSWORD);
		client.password = &password_utf8;
	}

	int rc = mqtt_connect(&client);
	zsock_freeaddrinfo(res);
	if (rc == 0) {
		char ipbuf[40] = {0};
		struct sockaddr_in *sin = (struct sockaddr_in *)client.broker;
		zsock_inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf));
		LOG_INF("MQTT TCP connected to %s:%d", ipbuf, MQTT_BROKER_PORT);
	} else {
		LOG_ERR("mqtt_connect() failed: %d", rc);
	}
	return rc;
}

int solar_mqtt_start(void)
{
	if (!started) {
		return -EINVAL;
	}

	int rc = mqtt_do_connect();
	if (rc) {
		return rc;
	}

	/* Start I/O thread first so CONNACK and keepalives are processed. */
	k_thread_create(&mqtt_io_thread_data, mqtt_io_thread_stack,
			K_THREAD_STACK_SIZEOF(mqtt_io_thread_stack), mqtt_io_worker,
			NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	LOG_INF("mqtt_io thread started");

	/* Start telemetry worker thread. */
	k_thread_create(&mqtt_thread_data, mqtt_thread_stack,
			K_THREAD_STACK_SIZEOF(mqtt_thread_stack), mqtt_worker,
			NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	LOG_INF("mqtt_worker thread started");

	return 0;
}