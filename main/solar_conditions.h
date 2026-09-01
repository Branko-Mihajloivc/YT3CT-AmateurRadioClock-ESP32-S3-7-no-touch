/*****************************************************************************
 * solar_conditions.h
 *
 * HF/VHF band-conditions panel, sourced from hamqsl.com's solarxml.php
 * feed: solar flux index, sunspot number, A/K index, and a 4-band
 * day/night condition table (80-40m, 30-20m, 17-15m, 12-10m). Ported from
 * the Arduino build's WiFiClientSecure/HTTPClient version to
 * esp_http_client, using ESP-IDF's built-in trusted-root certificate
 * bundle (`esp_crt_bundle_attach`) for proper TLS verification -- an
 * improvement over the Arduino build's `setInsecure()`, which skipped
 * certificate validation entirely.
 *
 * The fetch runs on its own dedicated core-0 FreeRTOS task, started
 * internally by solar_conditions_init() -- same reasoning as the Arduino
 * build: a multi-second TLS handshake blocking loop()/app_main() on the
 * same core as LVGL rendering and the RGB panel's vsync work was found to
 * cause display jitter there. Kept here even though the underlying cause
 * (Arduino's framework overhead eating scan-out margin, see
 * daynight_map.h) turned out not to apply the same way under ESP-IDF --
 * still sound practice to keep a multi-second network call off the
 * render-adjacent core.
 *****************************************************************************/
#pragma once

#include <lvgl.h>

/**
 * Creates the band-conditions column (as a child of `parent`, normally
 * lv_scr_act()) at (x, y), width `w`, height `h`, with placeholder "--"
 * values until the dedicated fetch task's first successful poll
 * (staggered a few seconds after boot, then hourly -- hamqsl.com's own
 * guidance). The stat+table block is vertically centered within `h`
 * below the title, so `h` should be the column's real available height,
 * not just a nominal/guessed value.
 */
void solar_conditions_init(lv_obj_t *parent, int x, int y, int w, int h);
