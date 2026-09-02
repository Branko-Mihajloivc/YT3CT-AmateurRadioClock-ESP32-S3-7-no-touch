#include "range_rings.h"
#include "daynight_map.h" // DAYNIGHT_MAP_W/H -- the map's own equirectangular pixel dimensions

#include <math.h>
#include <stdlib.h> // abs
#include <stdio.h>
#include "esp_heap_caps.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int W = DAYNIGHT_MAP_W;
static const int H = DAYNIGHT_MAP_H;

// Knjazevac, Serbia (the QTH) -- same coordinates city_clocks.cpp uses.
static const double QTH_LAT_DEG = 43.5675;
static const double QTH_LON_DEG = 22.2569;

static const double EARTH_RADIUS_KM = 6371.0;
static const double RING_STEP_KM = 1000.0;
// 20000km covers the whole planet from Knjazevac (max possible great-
// circle distance, antipodal, is ~20015km).
static const int NUM_RINGS = 20;
static const int BEARING_STEP_DEG = 5; // points per ring; smaller = smoother curve
static const int POINTS_PER_RING = 360 / BEARING_STEP_DEG + 1; // +1 to close the loop

// Compass-point spoke lines were tried and dropped for now (real hardware
// showed the same antimeridian/pole-crossing wraparound the rings had --
// see below -- plus, extended far enough to be useful, all 8 spokes
// converge toward the same antipodal point regardless of starting
// bearing, since every great-circle path from a point meets at its
// antipode). Worth revisiting as short fixed-length ticks near the QTH
// rather than full radiating lines, if wanted later.

static inline double deg2rad(double d) { return d * M_PI / 180.0; }
static inline double rad2deg(double r) { return r * 180.0 / M_PI; }

// Standard spherical "destination point given start, bearing, distance"
// formula (the direct geodetic problem) -- same one used by aviation/
// navigation great-circle calculators. lon2 is normalized to [-180,180).
static void dest_point(double lat1_deg, double lon1_deg, double bearing_deg, double dist_km,
                        double *lat2_deg, double *lon2_deg) {
    double lat1 = deg2rad(lat1_deg);
    double lon1 = deg2rad(lon1_deg);
    double theta = deg2rad(bearing_deg);
    double delta = dist_km / EARTH_RADIUS_KM;

    double lat2 = asin(sin(lat1) * cos(delta) + cos(lat1) * sin(delta) * cos(theta));
    double lon2 = lon1 + atan2(sin(theta) * sin(delta) * cos(lat1),
                                cos(delta) - sin(lat1) * sin(lat2));

    *lat2_deg = rad2deg(lat2);
    double lon2_deg_raw = rad2deg(lon2);
    // Wrap to [-180, 180)
    *lon2_deg = fmod(lon2_deg_raw + 540.0, 360.0) - 180.0;
}

static inline void project(double lat_deg, double lon_deg, lv_coord_t *sx, lv_coord_t *sy) {
    *sx = (lv_coord_t)((lon_deg + 180.0) / 360.0 * W);
    *sy = (lv_coord_t)((90.0 - lat_deg) / 180.0 * H);
}

static void draw_ring_segment(lv_obj_t *parent, lv_point_t *pts, int count) {
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, pts, count);
    lv_obj_set_style_line_width(line, 1, 0);
    lv_obj_set_style_line_color(line, lv_color_make(150, 150, 150), 0);
    lv_obj_set_style_line_opa(line, LV_OPA_50, 0);
    lv_obj_set_pos(line, 0, 0);
}

void range_rings_init(lv_obj_t *parent) {
    lv_point_t *points = (lv_point_t *)heap_caps_malloc(
        (size_t)NUM_RINGS * POINTS_PER_RING * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    if (!points) {
        printf("range_rings: PSRAM allocation failed\n");
        return;
    }

    for (int r = 0; r < NUM_RINGS; r++) {
        double dist_km = (r + 1) * RING_STEP_KM;
        lv_point_t *ring_points = points + (size_t)r * POINTS_PER_RING;

        for (int i = 0; i < POINTS_PER_RING; i++) {
            double bearing = (i * BEARING_STEP_DEG) % 360;
            double lat2, lon2;
            dest_point(QTH_LAT_DEG, QTH_LON_DEG, bearing, dist_km, &lat2, &lon2);
            project(lat2, lon2, &ring_points[i].x, &ring_points[i].y);
        }

        // Split into separate line segments wherever consecutive points
        // jump more than half the map's width. This happens once a
        // ring's radius is large enough to pass near a pole or cross the
        // +-180 antimeridian -- without splitting, lv_line draws one
        // continuous polyline that includes a spurious straight line
        // connecting what are actually opposite edges of the map
        // (confirmed on real hardware: looked like a curve, then a
        // straight line jumping clean across the screen).
        int seg_start = 0;
        for (int i = 1; i < POINTS_PER_RING; i++) {
            if (abs((int)ring_points[i].x - (int)ring_points[i - 1].x) > W / 2) {
                int seg_len = i - seg_start;
                if (seg_len >= 2) draw_ring_segment(parent, &ring_points[seg_start], seg_len);
                seg_start = i;
            }
        }
        int seg_len = POINTS_PER_RING - seg_start;
        if (seg_len >= 2) draw_ring_segment(parent, &ring_points[seg_start], seg_len);
    }
}
