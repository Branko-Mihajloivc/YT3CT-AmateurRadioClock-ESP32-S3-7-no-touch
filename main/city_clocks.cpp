#include "city_clocks.h"
#include "sun_position.h"
#include "config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct City {
    const char *name;
    const char *tz;   // POSIX TZ string
    double lat_deg;
    double lon_deg;
};

// Knjazevac is the home QTH (fixed here regardless of config.h's LOCAL_TZ,
// which describes wherever this particular board is physically running).
// Same order as the city markers baked into worldmap.bin by
// tools/make_worldmap.py -- keep the two in sync. Edit this table to
// change which cities are shown.
static const City CITIES[] = {
    { "Vancouver", "PST8PDT,M3.2.0,M11.1.0",          49.2827, -123.1207 },
    { "New York",  "EST5EDT,M3.2.0/2,M11.1.0/2",      40.7128,  -74.0060 },
    { "Knjazevac", "CET-1CEST,M3.5.0,M10.5.0/3",      43.5675,   22.2569 },
    { "Mumbai",    "IST-5:30",                         19.0760,   72.8777 },
    { "Sydney",    "AEST-10AEDT,M10.1.0/2,M4.1.0/3",  -33.8688,  151.2093 },
};
static const int NUM_CITIES = sizeof(CITIES) / sizeof(CITIES[0]);

static lv_obj_t *s_name_label[NUM_CITIES];
static lv_obj_t *s_combined_label[NUM_CITIES]; // "HH:MM  ^sunrise vsunset", recolored
static int s_last_shown_minute[NUM_CITIES];    // -1 == never set yet
static long s_utc_offset_sec[NUM_CITIES];      // this city's local time minus UTC, refreshed periodically -- see refresh_utc_offset()
// Cached "#ffc850 ^HH:MM vHH:MM#" portion. Needs room for: "#ffc850 " (8) +
// LV_SYMBOL_UP (3 bytes, UTF-8) + "HH:MM" (5) + " " (1) + LV_SYMBOL_DOWN
// (3 bytes) + "HH:MM" (5) + "#" (1) + nul = 27 bytes minimum -- the
// previous 24-byte buffer silently truncated (via snprintf) and cut off
// the last few bytes, which happened to be the sunset minutes and the
// closing "#". Sized with real headroom this time.
static char s_sun_str[NUM_CITIES][40];
static int s_knjazevac_x = 0; // set in city_clocks_init(), see city_clocks_get_knjazevac_x()

// Converts a UTC calendar date/time to a Unix epoch time_t using Howard
// Hinnant's days_from_civil algorithm, instead of relying on timegm()
// (not consistently available across ESP32 toolchain/newlib versions).
static time_t utc_civil_to_epoch(int year, int month, int day, int hour, int min, int sec) {
    int y = year - (month <= 2 ? 1 : 0);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days_since_epoch = era * 146097L + (long)doe - 719468L;
    return (time_t)(days_since_epoch * 86400L + hour * 3600L + min * 60L + sec);
}

// Runs `epoch` through the given POSIX TZ, then restores TZ to LOCAL_TZ --
// the existing UTC/local clock labels and configTzTime() state both assume
// LOCAL_TZ stays the active environment TZ between calls.
static void local_tm_in_tz(time_t epoch, const char *tz, struct tm *out) {
    setenv("TZ", tz, 1);
    tzset();
    localtime_r(&epoch, out);
    setenv("TZ", LOCAL_TZ, 1);
    tzset();
}

// utc_hours may be NAN (sun doesn't rise/set that day at that latitude --
// doesn't happen for any city in CITIES[], but handled cleanly).
// Takes the city's already-computed UTC offset (see refresh_utc_offset())
// rather than a TZ string -- plain epoch arithmetic + gmtime_r instead of
// another local_tm_in_tz() round trip, same reasoning as city_clocks_tick().
static void format_local_hm(const struct tm *utc_date, double utc_hours, long offset_sec,
                             char *out, size_t out_size) {
    if (isnan(utc_hours)) {
        snprintf(out, out_size, "--:--");
        return;
    }
    int hh = (int)utc_hours;
    int mm = (int)((utc_hours - hh) * 60.0 + 0.5);
    int day_offset = 0;
    if (mm >= 60) { mm -= 60; hh += 1; }
    if (hh >= 24) { hh -= 24; day_offset = 1; }

    // Convert the calendar date at midnight first, then add the
    // hour/minute (and any day rollover from the rounding above) as plain
    // epoch-second arithmetic -- avoids re-implementing month/year
    // rollover (e.g. Dec 31 -> Jan 1) by hand.
    time_t midnight_epoch = utc_civil_to_epoch(utc_date->tm_year + 1900, utc_date->tm_mon + 1,
                                                utc_date->tm_mday, 0, 0, 0);
    time_t epoch = midnight_epoch + day_offset * 86400L + hh * 3600L + mm * 60L;
    time_t local_epoch = epoch + offset_sec;
    struct tm local_tm;
    gmtime_r(&local_epoch, &local_tm);
    snprintf(out, out_size, "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
}

// Recomputes s_utc_offset_sec[i] via ONE setenv()/tzset() round trip.
// Deliberately not called every second -- see city_clocks_tick()'s
// comment for why.
static void refresh_utc_offset(int i, time_t now) {
    struct tm local_tm;
    local_tm_in_tz(now, CITIES[i].tz, &local_tm);
    // Reinterpret the local wall-clock fields as if they were UTC to get
    // an epoch value, then diff against the real UTC epoch -- gives the
    // raw offset (including whatever DST is in effect right now) without
    // needing to touch libc's TZ state again until this is next called.
    time_t local_as_utc_epoch = utc_civil_to_epoch(local_tm.tm_year + 1900, local_tm.tm_mon + 1,
                                                    local_tm.tm_mday, local_tm.tm_hour,
                                                    local_tm.tm_min, local_tm.tm_sec);
    s_utc_offset_sec[i] = (long)(local_as_utc_epoch - now);
}

// Rebuilds and sets city i's single combined label from the given local
// HH:MM plus whatever sun-times string is currently cached for it.
static void set_combined_text(int i, int hour, int min) {
    // 256, not 64: GCC's -Wformat-truncation (an error under ESP-IDF's
    // default build flags) can't prove s_sun_str[i]'s bound from its own
    // format strings (LV_SYMBOL_UP/DOWN are opaque const char* macros to
    // it), so it assumes a conservative worst case well past 40 bytes.
    // Genuinely safe either way -- s_sun_str is always properly bounded
    // -- this just gives the static analysis enough headroom to agree.
    char out[256];
    snprintf(out, sizeof(out), "%02d:%02d  %s", hour, min, s_sun_str[i]);
    lv_label_set_text(s_combined_label[i], out);
}

void city_clocks_init(lv_obj_t *parent, int x, int y, int w, int h) {
    // Dedicated bottom bar (not a floating overlay panel) -- one
    // equal-width column per city, both lines (name, and time + sunrise/
    // sunset) centered within the column.
    (void)h;
    const int col_w = w / NUM_CITIES;
    const int pad = 10;

    for (int i = 0; i < NUM_CITIES; i++) {
        int col_center_x = x + col_w * i + col_w / 2;
        if (strcmp(CITIES[i].name, "Knjazevac") == 0) s_knjazevac_x = col_center_x;
        s_last_shown_minute[i] = -1;
        snprintf(s_sun_str[i], sizeof(s_sun_str[i]), "#ffc850 %s--:-- %s--:--#",
                 LV_SYMBOL_UP, LV_SYMBOL_DOWN);

        // Fixed width for both boxes below, comfortably narrower than the
        // full column (col_w, which is what these used before and never
        // wrapped) but with a safe margin above this text pattern's
        // actual rendered width -- LVGL's dynamic content-based sizing
        // (LV_SIZE_CONTENT + measure) proved unreliable here, so this
        // trades a perfectly tight fit for something that just works.
        const int box_w = col_w - 20;
        int box_x = col_center_x - box_w / 2;

        s_combined_label[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(s_combined_label[i], lv_color_white(), 0);
        lv_obj_set_style_bg_color(s_combined_label[i], lv_color_make(20, 20, 20), 0);
        lv_obj_set_style_bg_opa(s_combined_label[i], LV_OPA_80, 0);
        lv_obj_set_style_radius(s_combined_label[i], 4, 0);
        lv_label_set_recolor(s_combined_label[i], true); // lets the "#ffc850 ...#" span color just the sun-times part amber
        lv_obj_set_width(s_combined_label[i], box_w);
        lv_obj_set_style_text_align(s_combined_label[i], LV_TEXT_ALIGN_CENTER, 0);
        char placeholder[256]; // see set_combined_text()'s comment on this same GCC -Wformat-truncation quirk
        snprintf(placeholder, sizeof(placeholder), "--:--  %s", s_sun_str[i]);
        lv_label_set_text(s_combined_label[i], placeholder);
        lv_obj_set_pos(s_combined_label[i], box_x, y + pad + 22);

        s_name_label[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(s_name_label[i], lv_color_make(190, 190, 190), 0);
        lv_obj_set_style_bg_color(s_name_label[i], lv_color_make(20, 20, 20), 0);
        lv_obj_set_style_bg_opa(s_name_label[i], LV_OPA_80, 0);
        lv_obj_set_style_radius(s_name_label[i], 4, 0);
        lv_obj_set_width(s_name_label[i], box_w); // same width as the combined label, not the full column
        lv_obj_set_style_text_align(s_name_label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(s_name_label[i], CITIES[i].name);
        lv_obj_set_pos(s_name_label[i], box_x, y + pad);
    }
}

void city_clocks_tick(time_t now) {
    for (int i = 0; i < NUM_CITIES; i++) {
        // Cached offset + gmtime_r instead of local_tm_in_tz()'s
        // setenv()/tzset() round trip -- this runs every second for all 5
        // cities, and repeated setenv() calls turned out to leak memory
        // on this toolchain's newlib (~15 bytes/call). At 10 calls/sec
        // (2 per city, switching to the city's TZ and back to LOCAL_TZ)
        // that added up to a ~9KB/minute leak, which eventually starved
        // WiFi's internal allocator and crashed the whole chip after
        // ~15-25 minutes of uptime. gmtime_r never touches the TZ env var
        // at all, so this path is leak-free regardless of call frequency.
        time_t local_epoch = now + s_utc_offset_sec[i];
        struct tm local_tm;
        gmtime_r(&local_epoch, &local_tm);

        // The label only shows HH:MM, so it's unchanged 59 seconds out of
        // every 60 -- skip the redraw (and the scattered-area buffer
        // flush that comes with it) when nothing actually changed.
        int minute_of_day = local_tm.tm_hour * 60 + local_tm.tm_min;
        if (minute_of_day == s_last_shown_minute[i]) continue;
        s_last_shown_minute[i] = minute_of_day;

        set_combined_text(i, local_tm.tm_hour, local_tm.tm_min);
    }
}

void city_clocks_update_sun_times(const struct tm *utc) {
    for (int i = 0; i < NUM_CITIES; i++) {
        refresh_utc_offset(i, time(nullptr)); // ~60s cadence -- see city_clocks_tick()'s comment for why this isn't done every second instead

        double sunrise_utc_hours, sunset_utc_hours;
        sun_rise_set(utc, CITIES[i].lat_deg, CITIES[i].lon_deg, &sunrise_utc_hours, &sunset_utc_hours);

        char rise_buf[8], set_buf[8];
        format_local_hm(utc, sunrise_utc_hours, s_utc_offset_sec[i], rise_buf, sizeof(rise_buf));
        format_local_hm(utc, sunset_utc_hours, s_utc_offset_sec[i], set_buf, sizeof(set_buf));

        snprintf(s_sun_str[i], sizeof(s_sun_str[i]), "#ffc850 %s%s %s%s#",
                 LV_SYMBOL_UP, rise_buf, LV_SYMBOL_DOWN, set_buf);

        // Rebuild the combined label immediately (using this city's
        // current local time) rather than waiting for the next tick, so
        // updated sun times don't sit stale for up to a second. Cached
        // offset + gmtime_r, same reasoning as city_clocks_tick().
        time_t local_epoch = time(nullptr) + s_utc_offset_sec[i];
        struct tm local_tm;
        gmtime_r(&local_epoch, &local_tm);
        set_combined_text(i, local_tm.tm_hour, local_tm.tm_min);
        s_last_shown_minute[i] = local_tm.tm_hour * 60 + local_tm.tm_min;
    }
}

int city_clocks_get_knjazevac_x(void) {
    return s_knjazevac_x;
}
