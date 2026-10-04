/* Full MQTT client: plain (non-TLS) connection with RSSI, Solar V, Battery V,
 * Temperature, and Humidity telemetry.
 * - resolves MQTT_BROKER_HOST
 * - connects to broker, sets LWT to solar_node/availability (offline retained)
 * - reads ADC channels for solar (J2) and battery (J3) with voltage divider math
 * - reads DHT22 on IO26 for temperature and humidity
 */

#include "mqtt_client.h"
#include "config.h"
#include "wifi_autoconnect.h"
#include "dht_sensor.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/devicetree.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(solar_mqtt, LOG_LEVEL_INF);

/* ADC Definitions & Divider Math */
#define DT_SPEC_SOLAR   ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), solar_v)
#define DT_SPEC_BATTERY ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), battery_v)

static const struct adc_dt_spec adc_solar = DT_SPEC_SOLAR;
static const struct adc_dt_spec adc_battery = DT_SPEC_BATTERY;

/* Voltage Divider Ratios: (R_top + R_bottom) / R_bottom */
#define SOLAR_DIVIDER_RATIO   12.506f
#define BATTERY_DIVIDER_RATIO 5.865f
#define ADC_OVERSAMPLE_COUNT  16

/* Buffers and client instance */
static struct mqtt_client client;
static uint8_t rx_buffer[512];
static uint8_t tx_buffer[512];

/* WiFi scan helpers */
static struct net_mgmt_event_callback scan_cb;
static struct k_sem scan_sem;
static int8_t last_rssi = INT8_MIN;

/* Worker thread that performs periodic scans, reads ADCs, and publishes telemetry */
static struct k_thread mqtt_thread_data;
K_THREAD_STACK_DEFINE(mqtt_thread_stack, 4096);

/* Separate MQTT I/O thread to process CONNACK, PINGRESP and disconnects */
static struct k_thread mqtt_io_thread_data;
K_THREAD_STACK_DEFINE(mqtt_io_thread_stack, 3072);

static bool started;
static volatile bool mqtt_connected = false;

static int mqtt_do_connect(void);

/* Reads raw ADC counts, averages 16 samples, and converts to actual scaled voltage.
 * adc_channel_setup_dt() must be called once at init before invoking this. */
static int read_channel_volts(const struct adc_dt_spec *spec, float divider_ratio, float *out_volts)
{
	if (!adc_is_ready_dt(spec)) {
		LOG_ERR("ADC device %s not ready", spec->dev->name);
		return -ENODEV;
	}

	int16_t sample_buf;
	struct adc_sequence sequence = {
		.buffer = &sample_buf,
		.buffer_size = sizeof(sample_buf),
	};

	adc_sequence_init_dt(spec, &sequence);

	int32_t val_sum = 0;
	int ret;
	for (int i = 0; i < ADC_OVERSAMPLE_COUNT; i++) {
		ret = adc_read(spec->dev, &sequence);
		if (ret < 0) {
			LOG_ERR("ADC read error (%d)", ret);
			return ret;
		}
		val_sum += sample_buf;
		k_usleep(100);
	}

	int32_t val_avg = val_sum / ADC_OVERSAMPLE_COUNT;
	int32_t val_mv = val_avg;

	ret = adc_raw_to_millivolts_dt(spec, &val_mv);
	if (ret < 0) {
		LOG_ERR("ADC raw to mV conversion failed (%d)", ret);
		return ret;
	}

	*out_volts = ((float)val_mv / 1000.0f) * divider_ratio;
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

static void publish_telemetry(float solar_v, float battery_v,
			      bool dht_ok, float temp_c, float humidity_pct)
{
	char payload[32];

	/* 1. RSSI */
	if (last_rssi != INT8_MIN) {
		snprintf(payload, sizeof(payload), "%d", last_rssi);
		mqtt_publish_str("solar_node/sensor/rssi/state", payload, false);
	}

	/* 2. Solar Voltage */
	snprintf(payload, sizeof(payload), "%.2f", solar_v);
	mqtt_publish_str("solar_node/sensor/solar_voltage/state", payload, false);

	/* 3. Battery Voltage */
	snprintf(payload, sizeof(payload), "%.2f", battery_v);
	mqtt_publish_str("solar_node/sensor/battery_voltage/state", payload, false);

	/* 4 & 5. Temperature and Humidity (skipped if DHT22 read failed) */
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

			/* Home Assistant Discovery */
			static const char disc_rssi[] =
				"{\"name\":\"RSSI\","
				"\"state_topic\":\"solar_node/sensor/rssi/state\","
				"\"unit_of_measurement\":\"dBm\","
				"\"device_class\":\"signal_strength\","
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_rssi\","
				"\"device\":{\"identifiers\":[\"solar_node\"],\"name\":\"Solar Node\"}}";

			static const char disc_solar[] =
				"{\"name\":\"Solar Voltage\","
				"\"state_topic\":\"solar_node/sensor/solar_voltage/state\","
				"\"unit_of_measurement\":\"V\","
				"\"device_class\":\"voltage\","
				"\"suggested_display_precision\":2,"
				"\"value_template\":\"{{ value }}\","
				"\"unique_id\":\"solar_node_solar_v\","
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

			/* Yield between each discovery publish — rapid back-to-back sends
			 * exhaust the TCP TX buffers and cause the broker to drop the connection. */
			mqtt_publish_str("homeassistant/sensor/solar_node/rssi/config", disc_rssi, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/solar_voltage/config", disc_solar, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/battery_voltage/config", disc_battery, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/temperature/config", disc_temp, true);
			k_msleep(100);
			mqtt_publish_str("homeassistant/sensor/solar_node/humidity/config", disc_humidity, true);

		} else {
			LOG_WRN("MQTT CONNACK failed (%d) rc=%d", evt->result, evt->param.connack.return_code);
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

		/* Read Analog Voltages */
		float solar_v = 0.0f;
		float battery_v = 0.0f;

		int err = read_channel_volts(&adc_solar, SOLAR_DIVIDER_RATIO, &solar_v);
		if (err) {
			LOG_ERR("Failed to read Solar Voltage: %d", err);
		}

		err = read_channel_volts(&adc_battery, BATTERY_DIVIDER_RATIO, &battery_v);
		if (err) {
			LOG_ERR("Failed to read Battery Voltage: %d", err);
		}

		/* Read DHT22 Temperature & Humidity */
		float temp_c = 0.0f;
		float humidity_pct = 0.0f;
		bool dht_ok = (dht_sensor_read(&temp_c, &humidity_pct) == 0);

		/* Publish all gathered telemetry */
		publish_telemetry(solar_v, battery_v, dht_ok, temp_c, humidity_pct);

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

	/* Set up ADC channels once at init rather than on every read */
	int adc_err = adc_channel_setup_dt(&adc_solar);
	if (adc_err < 0) {
		LOG_ERR("ADC solar channel setup failed (%d)", adc_err);
	}
	adc_err = adc_channel_setup_dt(&adc_battery);
	if (adc_err < 0) {
		LOG_ERR("ADC battery channel setup failed (%d)", adc_err);
	}

	dht_sensor_init();

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