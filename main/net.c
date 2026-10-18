#include "net.h"
#include "board_pins.h"
#include "csi_sense.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"

// ----> SET THESE to your 2.4 GHz Wi-Fi network <----
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

static const char *TAG = "net";
static esp_netif_t *s_sta_netif;
static esp_ping_handle_t s_ping;
static bool s_up;

static void ping_ok_cb(esp_ping_handle_t h, void *args)      { (void)h; (void)args; }
static void ping_timeout_cb(esp_ping_handle_t h, void *args) { (void)h; (void)args; }
static void ping_end_cb(esp_ping_handle_t h, void *args)     { (void)h; (void)args; }

static void start_ping(void)
{
    if (s_ping) return;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.gw.addr == 0) {
        ESP_LOGW(TAG, "no gateway yet, ping deferred");
        return;
    }
    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    target.type = ESP_IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = ip.gw.addr;  // endian-safe copy

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PING_INTERVAL_MS;
    cfg.timeout_ms = 200;
    cfg.data_size = 32;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_ok_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
        .cb_args = NULL,
    };
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ping_new_session failed");
        s_ping = NULL;
        return;
    }
    esp_ping_start(s_ping);
    ESP_LOGI(TAG, "pinging gateway every %d ms", PING_INTERVAL_MS);
}

static void stop_ping(void)
{
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying...");
        s_up = false;
        csi_sense_set_wifi_ok(false);
        stop_ping();
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "got IP, starting ping");
        s_up = true;
        csi_sense_set_wifi_ok(true);
        start_ping();
    }
}

void net_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;  // accept any home-router security

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // no power save: keeps ping/CSI rate steady
    ESP_ERROR_CHECK(esp_wifi_start());

    // One-time scan: list visible 2.4 GHz networks for diagnostics.
    {
        wifi_scan_config_t scan_cfg = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
        };
        if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            wifi_ap_record_t *aps = malloc(n * sizeof(wifi_ap_record_t));
            if (aps && esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
                ESP_LOGI(TAG, "scan: %d APs visible", n);
                for (int i = 0; i < n; i++) {
                    ESP_LOGI(TAG, "  '%s' ch=%d rssi=%d", aps[i].ssid, aps[i].primary, aps[i].rssi);
                }
            }
            free(aps);
        }
    }

    ESP_LOGI(TAG, "connecting to \"%s\"...", WIFI_SSID);
}

bool net_is_up(void) { return s_up; }
