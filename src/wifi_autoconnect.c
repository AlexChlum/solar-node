#include "wifi_autoconnect.h"
#include "config.h"
#include "wifi_autoconnect.h"
#include "config.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(wifi_autoconnect, LOG_LEVEL_INF);

static bool started;
static int64_t next_attempt_ms;
static struct net_mgmt_event_callback wifi_cb;

/* Retry timings */
#define INITIAL_DELAY_MS 2000
#define RETRY_MS 20000
#define QUICK_RETRY_MS 5000

static bool has_ipv4(void)
{
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        return false;
    }
    return net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

static void wifi_event_cb(struct net_mgmt_event_callback *cb,
                          uint64_t mgmt_event,
                          struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (!cb->info || cb->info_length != sizeof(struct wifi_status)) {
        return;
    }

    const struct wifi_status *st = (const struct wifi_status *)cb->info;
    int code = st->status;

    if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
        int mapped = code;
        if (mapped == -ETIMEDOUT) {
            mapped = WIFI_STATUS_CONN_TIMEOUT;
        }
        LOG_INF("WiFi connect result: %s (%d)", wifi_conn_status_txt(mapped), code);
        if (mapped != WIFI_STATUS_CONN_SUCCESS) {
            next_attempt_ms = k_uptime_get() + QUICK_RETRY_MS;
        }
    } else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        LOG_WRN("WiFi disconnect event code: %d", code);
        next_attempt_ms = k_uptime_get() + QUICK_RETRY_MS;
    }
}

static int do_connect(void)
{
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        return -ENODEV;
    }

    uint8_t ssid_buf[WIFI_SSID_MAX_LEN + 1] = {0};
    uint8_t psk_buf[WIFI_PSK_MAX_LEN + 1] = {0};
    size_t ssid_len = strlen(WIFI_SSID);
    size_t psk_len = strlen(WIFI_PASSWORD);

    if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN) {
        return -EINVAL;
    }

    memcpy(ssid_buf, WIFI_SSID, ssid_len);

    struct wifi_connect_req_params params = {
        .ssid = ssid_buf,
        .ssid_length = (uint8_t)ssid_len,
        .channel = WIFI_CHANNEL_ANY,
        .band = WIFI_FREQ_BAND_UNKNOWN,
        .mfp = WIFI_MFP_OPTIONAL,
        .timeout = 20,
    };

    if (psk_len > 0) {
        if (psk_len < WIFI_PSK_MIN_LEN || psk_len > WIFI_PSK_MAX_LEN) {
            return -EINVAL;
        }
        memcpy(psk_buf, WIFI_PASSWORD, psk_len);
        params.psk = psk_buf;
        params.psk_length = (uint8_t)psk_len;
        params.security = WIFI_SECURITY_TYPE_PSK;
    } else {
        params.security = WIFI_SECURITY_TYPE_NONE;
    }

    return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}

int wifi_autoconnect_start(void)
{
    if (started) {
        return 0;
    }

    net_mgmt_init_event_callback(&wifi_cb, wifi_event_cb,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);

    next_attempt_ms = k_uptime_get() + INITIAL_DELAY_MS;
    started = true;
    LOG_INF("Auto-connect manager started (SSID: %s)", WIFI_SSID);
    return 0;
}

void wifi_autoconnect_poll(void)
{
    if (!started) {
        return;
    }

    if (has_ipv4()) {
        return;
    }

    int64_t now = k_uptime_get();
    if (now < next_attempt_ms) {
        return;
    }

    int ret = do_connect();
    if (ret == 0 || ret == -EINPROGRESS || ret == -EALREADY) {
        LOG_INF("Auto-connect attempt issued (SSID: %s)", WIFI_SSID);
        next_attempt_ms = now + RETRY_MS;
    } else {
        LOG_WRN("Auto-connect request failed: %d", ret);
        next_attempt_ms = now + QUICK_RETRY_MS;
    }
}

bool wifi_autoconnect_is_connected(void)
{
    return has_ipv4();
}
