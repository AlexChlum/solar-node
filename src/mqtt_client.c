/* Full implementation: plain MQTT (non-secure) client that publishes RSSI.
 * - resolves MQTT_BROKER_HOST
 * - connects to broker, sets LWT to solar_node/availability (offline retained)
 */

#include "mqtt_client.h"
#include "config.h"
#include "wifi_autoconnect.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
/* Use cycle counter for simple randomness; avoid rand header unavailability. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(solar_mqtt, LOG_LEVEL_INF);

/* Buffers and client instance */
static struct mqtt_client client;
static uint8_t rx_buffer[512];
static uint8_t tx_buffer[512];

/* WiFi scan helpers */
static struct net_mgmt_event_callback scan_cb;
static struct k_sem scan_sem;
static int8_t last_rssi = INT8_MIN;

/* Worker thread that performs periodic scans and publishes RSSI */
static struct k_thread mqtt_thread_data;
K_THREAD_STACK_DEFINE(mqtt_thread_stack, 4096);

/* Separate MQTT I/O thread to process CONNACK, PINGRESP and disconnects */
static struct k_thread mqtt_io_thread_data;
K_THREAD_STACK_DEFINE(mqtt_io_thread_stack, 3072);

static bool started;
static volatile bool mqtt_connected = false;

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
            k_msleep(250);
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

static void publish_rssi_if_available(void)
{
    if (last_rssi == INT8_MIN) {
        LOG_DBG("No RSSI value available yet");
        return;
    }

    char payload[32];
    int len = snprintf(payload, sizeof(payload), "%d", last_rssi);

    struct mqtt_publish_param param;
    memset(&param, 0, sizeof(param));

    static const struct mqtt_utf8 topic = MQTT_UTF8_LITERAL("solar_node/sensor/rssi/state");
    param.message.topic.topic = topic;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;

    param.message.payload.data = (uint8_t *)payload;
    param.message.payload.len = len;

    param.message_id = (uint16_t)(k_cycle_get_32() & 0xffff);
    param.retain_flag = 0;

    /* Log the outgoing message contents for easier debugging */
    char topic_buf[128] = {0};
    memcpy(topic_buf, (const char *)param.message.topic.topic.utf8, MIN(sizeof(topic_buf)-1, param.message.topic.topic.size));
    LOG_INF("MQTT PUBLISH -> topic='%s' payload='%s' retain=%d", topic_buf, payload, param.retain_flag);

    int rc = mqtt_publish(&client, &param);
    if (rc) {
        LOG_WRN("mqtt_publish(rssi) failed: %d", rc);
    } else {
        LOG_INF("Published RSSI=%d", last_rssi);
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

            /* Publish availability=online (retained) */
            const struct mqtt_utf8 avail_topic = MQTT_UTF8_LITERAL("solar_node/availability");
            const char *online = "online";
            struct mqtt_publish_param p;
            memset(&p, 0, sizeof(p));
            p.message.topic.topic = avail_topic;
            p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
            p.message.payload.data = (uint8_t *)online;
            p.message.payload.len = 6;
            p.retain_flag = 1;
            int arc = mqtt_publish(&client, &p);
            LOG_INF("availability publish rc=%d", arc);

            /* Publish Home Assistant discovery for RSSI sensor (retained) */
            const char *disc = "{\"name\": \"Solar Node RSSI\", \"state_topic\": \"solar_node/sensor/rssi/state\", \"unit_of_measurement\": \"dBm\", \"value_template\": \"{{ value }}\", \"unique_id\": \"solar_node_rssi\", \"device\": {\"identifiers\": [\"solar_node\"], \"name\": \"Solar Node\"}}";
            struct mqtt_publish_param pd;
            memset(&pd, 0, sizeof(pd));
            static const struct mqtt_utf8 disc_topic = MQTT_UTF8_LITERAL("homeassistant/sensor/solar_node/rssi/config");
            pd.message.topic.topic = disc_topic;
            pd.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
            pd.message.payload.data = (uint8_t *)disc;
            pd.message.payload.len = strlen(disc);
            pd.retain_flag = 1;
            int drc = mqtt_publish(&client, &pd);
            LOG_INF("discovery publish rc=%d", drc);

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
        /* cb->info_length may vary; cast and be conservative */
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

        /* Trigger scan */
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
        LOG_INF("net_mgmt(NET_REQUEST_WIFI_SCAN) rc=%d", rc);
        if (rc) {
            LOG_WRN("Failed to request WiFi scan: %d", rc);
            k_msleep(5000);
            continue;
        }

        /* wait for scan done or timeout */
        if (k_sem_take(&scan_sem, K_SECONDS(10)) == 0) {
            publish_rssi_if_available();
        } else {
            LOG_WRN("WiFi scan timed out");
        }

        /* publish every 60 seconds */
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

    /* Register scan callbacks */
    net_mgmt_init_event_callback(&scan_cb, scan_event_cb,
                                 NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
    net_mgmt_add_event_callback(&scan_cb);

    started = true;
}

int solar_mqtt_start(void)
{
    if (!started) {
        return -EINVAL;
    }

    LOG_INF("solar_mqtt_start: connecting to %s:%d", MQTT_BROKER_HOST, MQTT_BROKER_PORT);

    struct zsock_addrinfo *res = NULL;
    char service[8];
    snprintf(service, sizeof(service), "%d", MQTT_BROKER_PORT);

    /* Try DNS resolution first */
    int gai = zsock_getaddrinfo(MQTT_BROKER_HOST, service, NULL, &res);
    if (gai == 0 && res) {
        /* Use first result - copy into static storage so pointer stays valid */
        struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
        static struct sockaddr_in broker_addr_static_dns;
        broker_addr_static_dns = *addr;
        broker_addr_static_dns.sin_port = htons(MQTT_BROKER_PORT);
        client.broker = (struct sockaddr *)&broker_addr_static_dns;
    } else {
        /* Fallback: try parsing MQTT_BROKER_HOST as numeric IPv4 address */
        struct sockaddr_in static_broker = {0};
        int p = zsock_inet_pton(AF_INET, MQTT_BROKER_HOST, &static_broker.sin_addr);
        if (p == 1) {
            static_broker.sin_family = AF_INET;
            static_broker.sin_port = htons(MQTT_BROKER_PORT);
            /* use a static variable so pointer remains valid */
            static struct sockaddr_in broker_addr_static;
            broker_addr_static = static_broker;
            client.broker = (struct sockaddr *)&broker_addr_static;
        } else {
            LOG_ERR("DNS resolution failed for %s: %d", MQTT_BROKER_HOST, gai);
            return -ENOTCONN;
        }
    }

    /* Client id */
    client.client_id = MQTT_UTF8_LITERAL(MQTT_CLIENT_ID);

    /* Username/password if provided */
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
    LOG_INF("mqtt_connect returned %d", rc);
    if (rc == 0 && client.broker) {
        char ipbuf[64] = {0};
        struct sockaddr *b = (struct sockaddr *)client.broker;
        if (b->sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)b;
            zsock_inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf));
            uint16_t port = sys_be16_to_cpu(sin->sin_port);
            LOG_INF("broker resolved: %s:%d", ipbuf, port);
        }
    }
    if (rc) {
        LOG_ERR("mqtt_connect() failed: %d", rc);
        return rc;
    }

    /* Start I/O thread first so CONNACK and keepalive are processed. */
    k_thread_create(&mqtt_io_thread_data, mqtt_io_thread_stack,
                    K_THREAD_STACK_SIZEOF(mqtt_io_thread_stack), mqtt_io_worker,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    LOG_INF("mqtt_io thread started");

    /* Start worker thread to scan and publish RSSI */
    k_thread_create(&mqtt_thread_data, mqtt_thread_stack,
                    K_THREAD_STACK_SIZEOF(mqtt_thread_stack), mqtt_worker,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    LOG_INF("mqtt_worker thread started");

    return 0;
}

int solar_mqtt_publish_test(const char *topic, const char *payload)
{
    if (!started) {
        LOG_WRN("MQTT publish requested but client not started");
        return -1;
    }

    struct mqtt_publish_param param;
    memset(&param, 0, sizeof(param));
    struct mqtt_utf8 t = { (const uint8_t *)topic, (uint32_t)strlen(topic) };
    param.message.topic.topic = t;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    param.message.payload.data = (uint8_t *)payload;
    param.message.payload.len = strlen(payload);
    param.retain_flag = 0;

    return mqtt_publish(&client, &param);
}
