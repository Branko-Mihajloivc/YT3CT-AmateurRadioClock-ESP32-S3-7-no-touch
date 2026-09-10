#include "wifi_manager.h"
#include "config.h"

#include <string.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_wifi_event_group;
static const int CONNECTED_BIT = BIT0;
static volatile bool s_connected = false;
static bool s_sntp_started = false;
static esp_netif_t *s_sta_netif = NULL;

// Plain esp_wifi_connect() retries alone weren't enough to recover from a
// real disconnect (confirmed on real hardware: WiFi went red and stayed
// disconnected -- state cycling init->auth->init every ~3s -- until the
// device was power-cycled; a fresh esp_wifi_init() cleared it every time,
// but the retry loop below never did). That's the classic signature of
// the WiFi driver getting stuck in a state esp_wifi_connect() alone can't
// clear -- needs a full stack restart. Rather than requiring a manual
// power cycle, do that restart automatically after enough consecutive
// failures.
static int s_disconnect_count = 0;
static const int WIFI_RESTART_THRESHOLD = 15; // ~15 retries at the driver's own ~3-4s reconnect cadence, roughly 50-60s of continuous disconnection

// Confirmed on real hardware (2026-09-04): esp_wifi_stop()/esp_wifi_start()
// alone is NOT enough to clear a genuinely stuck driver state -- watched it
// cycle through the restart-threshold path 9 times in a row (~40 minutes),
// failing with the identical init->auth->init rejection every single time,
// device absent from the router's client list throughout. The only thing
// that has ever cleared this in the past was a full power cycle -- i.e. a
// completely fresh boot, not just restarting the driver in place. So after
// a couple stack-restart cycles still haven't worked, stop trying to be
// clever and just reboot the whole device, matching what's actually been
// observed to work.
static int s_stack_restart_count = 0;
static const int WIFI_REBOOT_THRESHOLD = 2; // ~2 stack restarts (~3-5 min of continuous failure) before giving up and rebooting

// The retry-every-~3.4s cadence above turned out to have a real downside:
// confirmed on real hardware (2026-09-04) that during an extended outage,
// this hammered the AP with a reconnect attempt every ~3.4 seconds for
// several minutes straight, across two full stack restarts, and the
// device never got back into the router's client list at all -- not
// "connected but no internet", genuinely absent, as if the AP were
// ignoring/dropping it. The user's phone stayed connected the entire
// time on the same router (an ISP-provided EuroDOCSIS cable gateway,
// 2.4GHz only, no 5GHz to fall back to), so this wasn't a real network
// outage -- the leading theory is the router's own WiFi firmware
// rate-limiting or temporarily blocking a client that reconnects this
// aggressively. Backing off exponentially (instead of retrying at a
// constant fast cadence forever) is a lot less likely to look like abuse
// to a consumer-grade router's firmware.
static esp_timer_handle_t s_reconnect_timer = NULL;

// Learned the hard way (2026-09-10): this board's enclosure gets glued shut
// before every config.h detail gets triple-checked, and a wrong WIFI_SSID/
// WIFI_PASSWORD could otherwise stay silently wrong with no easy way back
// in short of finding another physical port. Cycling through a small list
// of known networks on every reconnect attempt means a wrong/unreachable
// primary network doesn't strand the device -- it'll fall back to trying
// the other one(s) instead of retrying the same dead network forever.
typedef struct {
    const char *ssid;
    const char *password;
} wifi_credential_t;

static const wifi_credential_t WIFI_CREDENTIALS[] = {
    { WIFI_SSID, WIFI_PASSWORD },
    { WIFI_SSID_FALLBACK, WIFI_PASSWORD_FALLBACK },
    { WIFI_SSID_FALLBACK2, WIFI_PASSWORD_FALLBACK2 },
};
static const int NUM_WIFI_CREDENTIALS = sizeof(WIFI_CREDENTIALS) / sizeof(WIFI_CREDENTIALS[0]);
static int s_wifi_cred_index = 0;

static void apply_wifi_credential(int index) {
    const wifi_credential_t *cred = &WIFI_CREDENTIALS[index];
    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, cred->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, cred->password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Using WiFi network %d/%d: '%s'", index + 1, NUM_WIFI_CREDENTIALS, cred->ssid);
}

static uint32_t reconnect_backoff_ms(int disconnect_count) {
    if (disconnect_count <= 3) return 0; // first few retries: same fast behavior as before, for ordinary transient blips
    int shift = disconnect_count - 4;
    if (shift > 5) shift = 5; // caps growth at 1000 * 2^5 = 32000, clamped to 30000 below anyway
    uint32_t delay_ms = 1000UL << shift; // 1s, 2s, 4s, 8s, 16s, 30s(capped)...
    return delay_ms > 30000UL ? 30000UL : delay_ms;
}

static void reconnect_timer_cb(void *arg) {
    (void)arg;
    esp_wifi_connect();
}

// solar_conditions' hamqsl.com fetch fails 100% of the time with a DNS
// lookup error (esp-tls: getaddrinfo() returns 202 = EAI_FAIL) even though
// the same hostname resolves fine from a PC -- but that PC test was over a
// completely different network path (wired ISP connection, its own DNS
// servers), not this WiFi link. The ESP32 has no DNS override and just
// takes whatever this WiFi network's own DHCP hands it, which is a
// plausible culprit for a resolver that's broken/restrictive for this one
// domain even though basic connectivity (ping, WiFi assoc) works fine.
// Force a known-good public resolver instead of trusting the WiFi
// network's own DNS.
static void apply_public_dns() {
    if (!s_sta_netif) return;
    esp_netif_dns_info_t dns;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
    esp_err_t e1 = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 4, 4);
    esp_err_t e2 = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);

    esp_netif_dns_info_t check;
    esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &check);
    ESP_LOGI(TAG, "apply_public_dns: set main=%s backup=%s, readback main=" IPSTR,
             esp_err_to_name(e1), esp_err_to_name(e2), IP2STR(&check.ip.u_addr.ip4));
}

// Diagnostic for the "is DNS broken network-wide or just for hamqsl.com"
// question -- NTP_SERVER_1/2 are hostnames too, so if this never fires,
// nothing on this WiFi network can resolve external domains at all.
static void on_sntp_sync(struct timeval *tv) {
    ESP_LOGI(TAG, "SNTP time sync received: tv_sec=%ld", (long)tv->tv_sec);
}

static void start_sntp_once() {
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST(NTP_SERVER_1, NTP_SERVER_2));
    sntp_config.sync_cb = on_sntp_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));
}

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
        s_disconnect_count++;
        if (NUM_WIFI_CREDENTIALS > 1) {
            s_wifi_cred_index = (s_wifi_cred_index + 1) % NUM_WIFI_CREDENTIALS;
            apply_wifi_credential(s_wifi_cred_index);
        }
        uint32_t delay_ms = reconnect_backoff_ms(s_disconnect_count);
        ESP_LOGW(TAG, "WiFi disconnected (attempt %d), retrying in %u ms...", s_disconnect_count, (unsigned)delay_ms);
        if (s_disconnect_count >= WIFI_RESTART_THRESHOLD) {
            s_disconnect_count = 0;
            s_stack_restart_count++;
            if (s_stack_restart_count >= WIFI_REBOOT_THRESHOLD) {
                ESP_LOGE(TAG, "Still disconnected after %d WiFi stack restarts -- rebooting the device", s_stack_restart_count);
                esp_restart();
            }
            ESP_LOGW(TAG, "Still disconnected after %d attempts -- restarting the WiFi stack (restart #%d)", WIFI_RESTART_THRESHOLD, s_stack_restart_count);
            esp_wifi_stop();
            esp_wifi_start(); // re-triggers WIFI_EVENT_STA_START below, which calls esp_wifi_connect() again
        } else if (delay_ms == 0) {
            esp_wifi_connect(); // first few retries: same fast behavior as before, for ordinary transient blips
        } else {
            esp_timer_stop(s_reconnect_timer); // defensive -- harmless if it wasn't already running
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_disconnect_count = 0;
        s_stack_restart_count = 0;
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
        apply_public_dns(); // DHCP just (re)set its own DNS servers -- override them every time, not just once
        start_sntp_once(); // only needs doing once -- SNTP keeps itself in sync afterward
    }
}

void wifi_manager_init(void) {
    esp_timer_create_args_t reconnect_timer_args = {};
    reconnect_timer_args.callback = &reconnect_timer_cb;
    reconnect_timer_args.name = "wifi_reconnect";
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer));

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    ESP_LOGI(TAG, "free internal RAM before esp_wifi_init(): %u bytes (largest block: %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_wifi_cred_index = 0;
    apply_wifi_credential(s_wifi_cred_index);
    ESP_ERROR_CHECK(esp_wifi_start());

    // Default modem-sleep power save periodically sends null-data-frame
    // keepalives to the AP; on a flaky link those retry in a tight ~100ms
    // loop for tens of seconds at a time (seen in the serial log as a burst
    // of "wifi:...null" warnings). This is a mains-powered clock, not a
    // battery device, so there's nothing to gain from power save -- and
    // this project has already learned the hard way (see the Arduino->
    // ESP-IDF port) that any sustained periodic radio/bus activity can
    // contend with the RGB panel's PSRAM-DMA scan-out and show up as a
    // visible glitch. Disable it outright.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to WiFi '%s'...", WIFI_SSID);
    // Block up to ~20s for the first connect, matching the Arduino build's
    // timeout -- WiFi_EVENT_STA_DISCONNECTED's handler keeps retrying in
    // the background afterward regardless of whether this succeeds.
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi connect timed out after 20s. This board has no RTC, so without "
                       "NTP the clock will keep showing whatever time it booted with -- it'll "
                       "self-correct once WiFi comes back.");
    }
}

bool wifi_manager_is_connected(void) {
    return s_connected;
}
