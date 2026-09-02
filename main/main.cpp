/*****************************************************************************
 * main.cpp -- GeochronClock, ESP-IDF port
 *
 * Ported from the original Arduino-framework build of this project after
 * an extensive elimination test traced a persistent display jitter/
 * ghosting glitch to the RGB panel's pixel clock not having enough
 * scan-out timing margin under Arduino -- every periodic application task
 * was individually ruled out first (see daynight_map.h), and a direct A/B
 * against Waveshare's own unmodified ESP-IDF LVGL demo (clean at 30.85MHz,
 * vs. this project's Arduino build never managing better than ~21MHz
 * without jitter) confirmed removing Arduino's framework overhead
 * recovers real margin.
 *
 * PHASE 1 (done): full visual layout -- top bar, band conditions column,
 * day/night map, bottom city-clocks bar -- ported and confirmed clean
 * (multiple reboots, no jitter) at 30.85MHz. This is what actually
 * mattered: proving this project's real visual complexity, not just
 * Waveshare's demo widgets, stays clean under ESP-IDF.
 *
 * PHASE 2 (done): WiFi (wifi_manager.cpp) + NTP wired up and confirmed
 * clean across multiple reboots, including WiFi's real Core-0 radio
 * workload -- not just the lighter Phase 1 test.
 *
 * PHASE 3 (current): the band-conditions HTTPS fetch (solar_conditions.cpp,
 * via esp_http_client + ESP-IDF's built-in cert bundle) and the DHT11
 * sensor read (env_sensor.cpp, a from-scratch native reimplementation of
 * the bit-banged protocol) are now wired up too -- full feature parity
 * with the Arduino build.
 *****************************************************************************/
#include <assert.h>
#include <time.h>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_netif_sntp.h"

// Waveshare's vendor headers (unmodified) don't wrap their declarations in
// extern "C" -- harmless for the demo's plain-C main.c, but this project's
// main.cpp is C++ (to share translation-unit linkage with the ported
// daynight_map.cpp/city_clocks.cpp/etc.), so the compiled .c
// implementations need C linkage spelled out here instead.
extern "C" {
#include "rgb_lcd_port.h"
#include "sd.h"
}

#include "daynight_map.h"
#include "city_clocks.h"
#include "solar_conditions.h"
#include "beacon_panel.h"
#include "range_rings.h"
#include "env_sensor.h"
#include "wifi_manager.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "geochronclock";

// Screen regions (1024x600 total). The map (DAYNIGHT_MAP_W/H in
// daynight_map.h) fills the entire screen; the top bar, band-conditions
// column, and bottom bar are text floating on top of it, not separate
// reserved areas, so these are just placement/wrap-width hints for that
// text, independent of the map's own size.
static const int TOP_BAR_H = 70;
static const int BAND_COL_W = 240; // solar_conditions.cpp positions its own columns off measured content width now; this just bounds the title's wrap width
static const int BOTTOM_BAR_H = 60;
static const int MAP_X = 0;
static const int MAP_Y = 0;

static lv_obj_t *local_time_label;
static lv_obj_t *utc_time_label;
static lv_obj_t *date_label;
static lv_obj_t *wifi_status_label;

// Same dark translucent highlight box style as the bottom bar's city
// labels (city_clocks.cpp) -- applied to a label with no explicit width
// set, so it hugs that label's own natural single-line content instead of
// needing a hand-guessed fixed width.
static void apply_highlight(lv_obj_t *label) {
    lv_obj_set_style_bg_color(label, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_80, 0);
    lv_obj_set_style_radius(label, 4, 0);
    lv_obj_set_style_pad_hor(label, 6, 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
}

// A birthday greeting for YT3SM, shown as soon as the panel/LVGL are up --
// before the SD map load, full UI build, and first heavy map recompute
// all start competing for the same core and PSRAM bus back-to-back.
// Doubles as this project's boot screen: gives the panel something steady
// to settle on first, and gives a human a few seconds to actually read
// it. No diacritics -- the bundled Montserrat fonts only cover Basic
// Latin + Latin-1 Supplement, not the Latin Extended-A range Serbian's
// accented letters live in.
static void show_boot_screen() {
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

        lv_obj_t *to_label = lv_label_create(scr);
        lv_obj_set_style_text_color(to_label, lv_color_make(180, 180, 180), 0);
#if LV_FONT_MONTSERRAT_30
        lv_obj_set_style_text_font(to_label, &lv_font_montserrat_30, 0);
#endif
        lv_label_set_text(to_label, "Dragom prijatelju YT3SM,");
        lv_obj_align(to_label, LV_ALIGN_CENTER, 0, -150);

        lv_obj_t *title_label = lv_label_create(scr);
        lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_48
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_48, 0);
#elif LV_FONT_MONTSERRAT_30
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_30, 0);
#endif
        lv_label_set_text(title_label, "Srecan rodjendan!");
        lv_obj_align(title_label, LV_ALIGN_CENTER, 0, -70);

        lv_obj_t *body_label = lv_label_create(scr);
        lv_obj_set_style_text_color(body_label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_30
        lv_obj_set_style_text_font(body_label, &lv_font_montserrat_30, 0);
#endif
        lv_obj_set_style_text_align(body_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(body_label,
            "Neka ti signal uvek bude jak, smetnje\n"
            "niske, a QSO veze brojne i daleke. 73!");
        lv_obj_align(body_label, LV_ALIGN_CENTER, 0, 30);

        lv_obj_t *from_label = lv_label_create(scr);
        lv_obj_set_style_text_color(from_label, lv_color_make(180, 180, 180), 0);
#if LV_FONT_MONTSERRAT_30
        lv_obj_set_style_text_font(from_label, &lv_font_montserrat_30, 0);
#endif
        lv_label_set_text(from_label, "- " CALLSIGN);
        lv_obj_align(from_label, LV_ALIGN_CENTER, 0, 140);

        esp_lv_adapter_unlock();
    }

    // Same double-buffer convergence dance daynight_map.cpp's map_task
    // uses after each terminator update -- direct mode only paints into
    // whichever physical buffer is active, so this needs two flips before
    // both buffers agree on this frame.
    for (int i = 0; i < 2; i++) {
        if (esp_lv_adapter_lock(100) == ESP_OK) {
            lv_obj_invalidate(lv_scr_act());
            esp_lv_adapter_unlock();
        } else {
            ESP_LOGW(TAG, "lock timeout at boot-screen invalidate #%d", i);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(6000)); // hold the greeting on screen long enough to actually read
}

static void build_ui(bool map_ok) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // ---- top bar: DATE | LOC <time> | UTC <time> | Temp | WiFi ----
    // Callsign moved out to its own spot centered over Knjazevac (the
    // QTH), above the bottom bar -- see below. It was the single widest
    // element here (48pt), and callsign+date+LOC+UTC+Temp+WiFi together
    // never fit in 1024px on real hardware; swapping it for a shorter
    // date group in the same slot freed enough room.
#if LV_FONT_MONTSERRAT_30
#define TOP_BAR_FONT &lv_font_montserrat_30
#else
#define TOP_BAR_FONT NULL
#endif

    date_label = lv_label_create(scr);
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    if (TOP_BAR_FONT) lv_obj_set_style_text_font(date_label, TOP_BAR_FONT, 0);
    lv_obj_set_pos(date_label, 20, 20);
    lv_label_set_text(date_label, "02.09.2026"); // real sample text for measurement, same trick the time labels below use
    apply_highlight(date_label);

    // Everything after date is chained with align_to off the PREVIOUS
    // element's actual rendered right edge instead of a hand-picked
    // absolute x -- see the Arduino build's history for why. Each time
    // label is temporarily filled with "23:59:59" (real digits, wider
    // than the "--:--:--" placeholder) while the NEXT element's position
    // is measured off it, then reset back to the placeholder -- date
    // gets the same treatment below, once loc_suffix has aligned off it.
    static const int GROUP_GAP = 35;
    static const int LABEL_GAP = 8;

    lv_obj_t *loc_suffix = lv_label_create(scr);
    lv_obj_set_style_text_color(loc_suffix, lv_color_make(150, 150, 150), 0);
    if (TOP_BAR_FONT) lv_obj_set_style_text_font(loc_suffix, TOP_BAR_FONT, 0);
    lv_label_set_text(loc_suffix, "LOC");
    apply_highlight(loc_suffix);
    lv_obj_align_to(loc_suffix, date_label, LV_ALIGN_OUT_RIGHT_MID, GROUP_GAP, 0);
    lv_label_set_text(date_label, "--- -- ---"); // now safe to reset -- loc_suffix already measured off the real sample text above

    local_time_label = lv_label_create(scr);
    lv_obj_set_style_text_color(local_time_label, lv_color_white(), 0);
    if (TOP_BAR_FONT) lv_obj_set_style_text_font(local_time_label, TOP_BAR_FONT, 0);
    lv_label_set_text(local_time_label, "23:59:59");
    apply_highlight(local_time_label);
    lv_obj_align_to(local_time_label, loc_suffix, LV_ALIGN_OUT_RIGHT_MID, LABEL_GAP, 0);

    lv_obj_t *utc_suffix = lv_label_create(scr);
    lv_obj_set_style_text_color(utc_suffix, lv_color_make(150, 150, 150), 0);
    if (TOP_BAR_FONT) lv_obj_set_style_text_font(utc_suffix, TOP_BAR_FONT, 0);
    lv_label_set_text(utc_suffix, "UTC");
    apply_highlight(utc_suffix);
    lv_obj_align_to(utc_suffix, local_time_label, LV_ALIGN_OUT_RIGHT_MID, GROUP_GAP, 0);
    lv_label_set_text(local_time_label, "--:--:--");

    utc_time_label = lv_label_create(scr);
    lv_obj_set_style_text_color(utc_time_label, lv_color_white(), 0);
    if (TOP_BAR_FONT) lv_obj_set_style_text_font(utc_time_label, TOP_BAR_FONT, 0);
    lv_label_set_text(utc_time_label, "23:59:59");
    apply_highlight(utc_time_label);
    lv_obj_align_to(utc_time_label, utc_suffix, LV_ALIGN_OUT_RIGHT_MID, LABEL_GAP, 0);

    // 4th group: temperature/humidity, chained off wherever the UTC time
    // value actually ended up.
    lv_obj_update_layout(utc_time_label);
    int temp_x = lv_obj_get_x(utc_time_label) + lv_obj_get_width(utc_time_label) + GROUP_GAP;
    env_sensor_init(scr, temp_x, 20);
    lv_label_set_text(utc_time_label, "--:--:--");

    // 5th (last) group: just "WiFi", colored green/red for connected/
    // disconnected. Fixed content (never resizes), so it's anchored to
    // the right edge with the same 20px margin the callsign uses on the
    // left, mirroring the callsign's column.
    wifi_status_label = lv_label_create(scr);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_make(255, 90, 90), 0);
    if (TOP_BAR_FONT) lv_obj_set_style_text_font(wifi_status_label, TOP_BAR_FONT, 0);
    lv_label_set_text(wifi_status_label, "WiFi");
    apply_highlight(wifi_status_label);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_RIGHT, -20, 20);
#undef TOP_BAR_FONT

    // ---- left column: band conditions ----
    solar_conditions_init(scr, 0, TOP_BAR_H, BAND_COL_W, (600 - BOTTOM_BAR_H) - TOP_BAR_H);

    // ---- right column: NCDXF beacon schedule (mirrors the left column) ----
    // Shifted 30px further left than a plain mirror of the left column --
    // its 3-column layout (freq/callsign/location) needs more horizontal
    // room than BAND_COL_W alone; without this, the widest location
    // strings ("Venezuela", "Argentina", "N.Zealand") clipped against the
    // screen's right edge on real hardware.
    beacon_panel_init(scr, 1024 - BAND_COL_W - 30, TOP_BAR_H, BAND_COL_W, (600 - BOTTOM_BAR_H) - TOP_BAR_H);

    // ---- bottom bar: city clocks ----
    city_clocks_init(scr, 0, 600 - BOTTOM_BAR_H, 1024, BOTTOM_BAR_H);

    // ---- callsign, centered over the Knjazevac city-clock column, just
    // above the bottom bar ---- moved out of the top bar to make room for
    // the date field there (see the top-bar comment above). Same 48pt
    // size as before, just relocated. Centered on
    // city_clocks_get_knjazevac_x() (the column's real on-screen center,
    // from city_clocks_init() just above) rather than Knjazevac's actual
    // lat/lon -- city_clocks.cpp lays out cities in equal-width columns by
    // index, not by true longitude, so the two don't match (confirmed on
    // real hardware: geographic placement put this visibly right of the
    // "Knjazevac" label itself). Static content (never changes at
    // runtime), so this is positioned once here rather than needing
    // refresh_clocks() to touch it.
    {
        lv_obj_t *callsign_label = lv_label_create(scr);
        lv_obj_set_style_text_color(callsign_label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_48
        lv_obj_set_style_text_font(callsign_label, &lv_font_montserrat_48, 0);
#elif LV_FONT_MONTSERRAT_30
        lv_obj_set_style_text_font(callsign_label, &lv_font_montserrat_30, 0);
#endif
        lv_label_set_text(callsign_label, CALLSIGN);
        apply_highlight(callsign_label);
        lv_obj_update_layout(callsign_label);
        int callsign_w = lv_obj_get_width(callsign_label);
        int callsign_h = lv_obj_get_height(callsign_label);
        lv_obj_set_pos(callsign_label, city_clocks_get_knjazevac_x() - callsign_w / 2, (600 - BOTTOM_BAR_H) - callsign_h - 10);
    }

    // ---- SD-card-missing warning ----
    lv_obj_t *status_label = lv_label_create(scr);
    lv_obj_set_style_text_color(status_label, lv_color_make(255, 90, 90), 0);
    lv_obj_set_style_bg_color(status_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(status_label, 6, 0);
    lv_obj_set_width(status_label, 600);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(status_label, 362, 270);
    if (!map_ok) {
        lv_label_set_text(status_label,
            "worldmap.bin not found on the SD card. "
            "Showing a placeholder background.");
    } else {
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// The day/night map recompute and its buffer-convergence dance live
// entirely in daynight_map.cpp's own core-0 task -- see daynight_map.h.
// This just handles the cheap, per-second stuff: the UTC/local time
// labels, WiFi status color, and each city's displayed HH:MM.
static void refresh_clocks() {
    time_t now = time(nullptr);
    struct tm utc_tm;
    gmtime_r(&now, &utc_tm);
    struct tm local_tm;
    localtime_r(&now, &local_tm);

    char utc_buf[16], local_buf[16], date_buf[16];
    strftime(utc_buf, sizeof(utc_buf), "%H:%M:%S", &utc_tm);
    strftime(local_buf, sizeof(local_buf), "%H:%M:%S", &local_tm);
    strftime(date_buf, sizeof(date_buf), "%d.%m.%Y", &local_tm); // e.g. "02.09.2026" -- local date, matches LOC time
    bool wifi_ok = wifi_manager_is_connected();

    if (esp_lv_adapter_lock(200) == ESP_OK) {
        lv_label_set_text(utc_time_label, utc_buf);
        lv_label_set_text(local_time_label, local_buf);
        lv_label_set_text(date_label, date_buf);
        lv_obj_set_style_text_color(wifi_status_label,
            wifi_ok ? lv_color_make(120, 220, 120) : lv_color_make(255, 90, 90), 0);
        city_clocks_tick(now);
        beacon_panel_update(now); // cheap -- fine every second even though the schedule only changes every 10s
        esp_lv_adapter_unlock();
    } else {
        ESP_LOGW(TAG, "lock timeout at refresh_clocks (per-second tick)");
    }
}

extern "C" void app_main(void)
{
    // ESP-IDF's SNTP component syncs the system clock but doesn't set the
    // local timezone the way the Arduino build's configTzTime() did --
    // set it explicitly and early, since city_clocks.cpp's per-city
    // offset math assumes LOCAL_TZ stays the active environment TZ.
    setenv("TZ", LOCAL_TZ, 1);
    tzset();

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_touch_handle_t tp_handle = NULL; // this board is a non-touch build

    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init(tear_mode, rotation, &panel_handle, &tp_handle));
    wavesahre_rgb_lcd_bl_on();

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel_handle, NULL, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, rotation);
    disp_config.profile.use_psram = true;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != NULL);

    if (tp_handle != NULL) {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp_handle);
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != NULL);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    show_boot_screen();

    // WiFi init needs a real chunk of internal (non-PSRAM) RAM for its RX
    // buffers -- doing this BEFORE the heavy UI construction below (the
    // map's PSRAM framebuffers are fine, but ~40 LVGL label objects and
    // their style cache eat real internal RAM too) avoids an
    // ESP_ERR_NO_MEM abort during esp_wifi_init() that showed up when
    // this ran after build_ui().
    wifi_manager_init(); // blocks up to ~20s for the first connect attempt, then keeps retrying in the background regardless
    ESP_LOGI(TAG, "Waiting for NTP time sync...");
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) != ESP_OK) {
        ESP_LOGW(TAG, "NTP sync timed out after 20s -- will keep syncing in the background once it's reachable.");
    }

    bool sd_ok = (sd_mmc_init() == ESP_OK);
    if (!sd_ok) {
        ESP_LOGW(TAG, "SD card not found/mounted -- put worldmap.bin on a FAT32 SD card. Continuing with a placeholder map.");
    }

    bool map_ok = false;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_obj_clean(lv_scr_act()); // drop the boot screen's labels before building the real UI
        map_ok = daynight_map_init(lv_scr_act(), MAP_X, MAP_Y, sd_ok);
        range_rings_init(lv_scr_act());
        build_ui(map_ok);
        esp_lv_adapter_unlock();
    }

    // First draw of the cheap per-second stuff; the map's own first
    // terminator draw happens separately via daynight_map.cpp's own
    // core-0 task.
    refresh_clocks();

    ESP_LOGI(TAG, "GeochronClock (ESP-IDF port, phase 2) running");

    // app_main() just keeps running as this project's per-second tick,
    // same cadence as the Arduino build's loop().
    while (true) {
        refresh_clocks();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
