#include "beacon_panel.h"
#include "daynight_map.h" // DAYNIGHT_MAP_W/H -- the map's own equirectangular pixel dimensions

#include <stdio.h>

// Official NCDXF transmission order. Lat/lon kept here (not just in a
// separate map-overlay module) so a future map-marker feature can share
// this single source of truth instead of duplicating the beacon list.
const beacon_t BEACONS[NUM_BEACONS] = {
    { "4U1UN",  "UN/NYC",    41.0f,  -74.0f },
    { "VE8AT",  "Canada",    80.0f,  -86.0f },
    { "W6WX",   "USA",       37.0f, -122.0f },
    { "KH6RS",  "Hawaii",    21.0f, -156.0f },
    { "ZL6B",   "N.Zealand",-41.0f,  176.0f },
    { "VK6RBP", "Australia",-32.0f,  116.0f },
    { "JA2IGY", "Japan",     34.0f,  137.0f },
    { "RR9O",   "Russia",    55.0f,   83.0f },
    { "VR2B",   "HongKong",  22.0f,  114.0f },
    { "4S7B",   "SriLanka",   7.0f,   80.0f },
    { "ZS6DN",  "S.Africa", -26.0f,   28.0f },
    { "5Z4B",   "Kenya",     -1.0f,   37.0f },
    { "4X6TU",  "Israel",    32.0f,   35.0f },
    { "OH2B",   "Finland",   60.0f,   25.0f },
    { "CS3B",   "Madeira",   33.0f,  -17.0f },
    { "LU4AA",  "Argentina",-35.0f,  -58.0f },
    { "OA4B",   "Peru",     -12.0f,  -77.0f },
    { "YV5B",   "Venezuela",  9.0f,  -68.0f },
};

static const char *BAND_LABELS[5] = { "14.100", "18.110", "21.150", "24.930", "28.200" };

static lv_obj_t *s_callsign_label[5];
static lv_obj_t *s_location_label[5];

// Map markers: one small persistent dot per beacon (position never
// changes -- these are fixed real-world locations), plus one callsign
// label per beacon that's normally hidden and only shown for whichever
// 5 beacons are currently active, right next to the dot.
static lv_obj_t *s_map_dot[NUM_BEACONS];
static lv_obj_t *s_map_label[NUM_BEACONS];
static int s_active_idx[5]; // which beacon index is on each band, from the last update

// Same dark translucent highlight box style used everywhere else in this
// UI (main.cpp's apply_highlight, city_clocks.cpp, solar_conditions.cpp)
// -- duplicated locally rather than shared across files, matching how
// those files already do it.
static void apply_highlight(lv_obj_t *label) {
    lv_obj_set_style_bg_color(label, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_80, 0);
    lv_obj_set_style_radius(label, 4, 0);
    lv_obj_set_style_pad_hor(label, 6, 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
}

// Measured WITH apply_highlight()'s padding applied first -- sizing a box
// from an unpadded measurement leaves the box's own padding with nowhere
// to go, silently wrapping the text (see solar_conditions.cpp's
// measure_box_width() comment for the real-hardware bug this caused
// there).
static int measure_box_width(lv_obj_t *parent, const char *text) {
    lv_obj_t *tmp = lv_label_create(parent);
    apply_highlight(tmp);
    lv_label_set_text(tmp, text);
    lv_obj_update_layout(tmp);
    int w = lv_obj_get_width(tmp);
    lv_obj_del(tmp);
    return w;
}

// Widest of `count` real strings, not one hand-picked reference -- a
// version that only measured "14.100" wrapped "24.930"/"28.200" onto a
// second line on real hardware (confirmed via photo): in this
// proportional font, digits like 9/3/2/8 render wider than 1/4/0, so a
// short string with "narrow" digits understates the width strings with
// "wide" digits actually need.
static int measure_box_width(lv_obj_t *parent, const char *text);
static int measure_max_box_width(lv_obj_t *parent, const char *const *texts, int count) {
    int max_w = 0;
    for (int i = 0; i < count; i++) {
        int w = measure_box_width(parent, texts[i]);
        if (w > max_w) max_w = w;
    }
    return max_w;
}

static int measure_row_height(lv_obj_t *parent, const char *text) {
    lv_obj_t *tmp = lv_label_create(parent);
    apply_highlight(tmp);
    lv_label_set_text(tmp, text);
    lv_obj_update_layout(tmp);
    int h = lv_obj_get_height(tmp);
    lv_obj_del(tmp);
    return h;
}

void beacon_panel_init(lv_obj_t *parent, int x, int y, int w, int h) {
    (void)w;
    const int pad = 14;
    const int gap = 10;
    const int row_step = measure_row_height(parent, "18.110") + 2;

    int cx = x + pad;
    int cy = y + pad;

    // 5 rows (14.100 down to 28.200), vertically centered in the column
    // the same way solar_conditions.cpp centers its stat+table block.
    int block_h = row_step * 5;
    int remaining_h = (y + h) - cy;
    int content_top = cy + (remaining_h > block_h ? (remaining_h - block_h) / 2 : 0);
    cy = content_top - row_step; // shifted up one row per request, on top of centering

    static const char *loc_texts[NUM_BEACONS];
    static const char *call_texts[NUM_BEACONS];
    for (int i = 0; i < NUM_BEACONS; i++) {
        loc_texts[i] = BEACONS[i].location;
        call_texts[i] = BEACONS[i].callsign;
    }
    int freq_w = measure_max_box_width(parent, BAND_LABELS, 5);
    int call_w = measure_max_box_width(parent, call_texts, NUM_BEACONS);
    int loc_w = measure_max_box_width(parent, loc_texts, NUM_BEACONS);
    int col_freq_x = cx;
    int col_call_x = col_freq_x + freq_w + gap;
    int col_loc_x = col_call_x + call_w + gap;

    for (int i = 0; i < 5; i++) {
        int row_y = cy + row_step * i;

        lv_obj_t *freq_label = lv_label_create(parent);
        lv_obj_set_style_text_color(freq_label, lv_color_make(230, 200, 60), 0); // same yellow as solar_conditions.cpp's headers
        apply_highlight(freq_label);
        lv_obj_set_width(freq_label, freq_w);
        lv_label_set_text(freq_label, BAND_LABELS[i]);
        lv_obj_set_pos(freq_label, col_freq_x, row_y);

        s_callsign_label[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(s_callsign_label[i], lv_color_white(), 0);
        apply_highlight(s_callsign_label[i]);
        lv_obj_set_width(s_callsign_label[i], call_w);
        lv_label_set_text(s_callsign_label[i], "--");
        lv_obj_set_pos(s_callsign_label[i], col_call_x, row_y);

        s_location_label[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(s_location_label[i], lv_color_make(190, 190, 190), 0);
        apply_highlight(s_location_label[i]);
        lv_obj_set_width(s_location_label[i], loc_w);
        lv_label_set_text(s_location_label[i], "--");
        lv_obj_set_pos(s_location_label[i], col_loc_x, row_y);
    }

    // Map markers: a small dot at every beacon's real location (same
    // equirectangular projection daynight_map.cpp uses for its subsolar
    // marker, so these align with the actual map image), dim by default.
    // Callsign labels are created but hidden -- beacon_panel_update()
    // shows/positions one only for whichever beacons are currently
    // active, so the map doesn't stay cluttered with all 18 all the time.
    for (int i = 0; i < NUM_BEACONS; i++) {
        int mx = (int)((BEACONS[i].lon + 180.0f) / 360.0f * DAYNIGHT_MAP_W);
        int my = (int)((90.0f - BEACONS[i].lat) / 180.0f * DAYNIGHT_MAP_H);

        s_map_dot[i] = lv_obj_create(parent);
        lv_obj_remove_style_all(s_map_dot[i]); // no default LVGL button/border chrome
        lv_obj_set_size(s_map_dot[i], 7, 7);
        lv_obj_set_style_radius(s_map_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_map_dot[i], lv_color_make(120, 120, 120), 0);
        lv_obj_set_style_bg_opa(s_map_dot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_map_dot[i], 1, 0);
        lv_obj_set_style_border_color(s_map_dot[i], lv_color_black(), 0);
        lv_obj_set_pos(s_map_dot[i], mx - 3, my - 3); // centered on the point

        s_map_label[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(s_map_label[i], lv_color_make(255, 220, 100), 0);
        apply_highlight(s_map_label[i]);
        lv_label_set_text(s_map_label[i], BEACONS[i].callsign);
        lv_obj_set_pos(s_map_label[i], mx + 6, my - 10);
        lv_obj_add_flag(s_map_label[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void beacon_panel_update(time_t utc_now) {
    if (!s_callsign_label[0]) return; // not yet initialized

    int slot = (int)((utc_now / 10) % NUM_BEACONS);

    // Un-highlight and hide the labels for whichever beacons were active
    // last cycle, before computing this cycle's set -- otherwise a
    // beacon that just rotated off stays lit on the map.
    for (int band = 0; band < 5; band++) {
        int old_idx = s_active_idx[band];
        if (s_map_dot[old_idx]) {
            lv_obj_set_style_bg_color(s_map_dot[old_idx], lv_color_make(120, 120, 120), 0);
            lv_obj_add_flag(s_map_label[old_idx], LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int band = 0; band < 5; band++) {
        int idx = ((slot - band) % NUM_BEACONS + NUM_BEACONS) % NUM_BEACONS;
        lv_label_set_text(s_callsign_label[band], BEACONS[idx].callsign);
        lv_label_set_text(s_location_label[band], BEACONS[idx].location);
        s_active_idx[band] = idx;

        lv_obj_set_style_bg_color(s_map_dot[idx], lv_color_make(90, 210, 90), 0); // same green as "Good" band conditions
        lv_obj_clear_flag(s_map_label[idx], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_map_label[idx]); // in case two active beacons' labels overlap
    }
}
