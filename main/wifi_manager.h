/*****************************************************************************
 * wifi_manager.h
 *
 * WiFi station connect (native esp_wifi, ported from the Arduino build's
 * WiFi.h usage) plus SNTP time sync. Connects once at boot and keeps
 * retrying in the background via WiFi event handlers if the initial
 * connect fails or drops later -- same behavior as the Arduino build's
 * connect_wifi() + the retry check in loop(), just event-driven instead
 * of polled.
 *****************************************************************************/
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Brings up NVS (required by the WiFi driver), the default event loop and
 * netif, connects to config.h's WIFI_SSID/WIFI_PASSWORD, and starts SNTP
 * against NTP_SERVER_1/NTP_SERVER_2 once connected. Blocks up to ~20s for
 * the first connect attempt (matching the Arduino build's timeout) before
 * returning either way -- WiFi keeps trying in the background afterward
 * regardless of whether this first attempt succeeded.
 */
void wifi_manager_init(void);

/** True if currently associated with the AP and holding an IP. */
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
