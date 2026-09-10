/*****************************************************************************
 * beacon_panel.h
 *
 * NCDXF/IARU International Beacon Project readout: which of the 18
 * beacon stations is transmitting on each of the 5 HF beacon frequencies
 * right now. No network, no ephemeris table -- purely a function of the
 * clock's own accurate time, verified against a real working open-source
 * ham-radio clock project's implementation of the same schedule (see
 * README's Acknowledgements for the citation):
 *
 *   slot = floor(epoch_seconds / 10) % 18
 *   beacon on 14.100 MHz = BEACONS[slot]
 *   beacon on 18.110 MHz = BEACONS[(slot - 1 + 18) % 18]
 *   beacon on 21.150 MHz = BEACONS[(slot - 2 + 18) % 18]
 *   beacon on 24.930 MHz = BEACONS[(slot - 3 + 18) % 18]
 *   beacon on 28.200 MHz = BEACONS[(slot - 4 + 18) % 18]
 *
 * i.e. each beacon holds a 10-second slot on 14.100 MHz, then steps up to
 * the next higher band for the next 10 seconds while a new beacon takes
 * over 14.100 -- so all 5 bands are simultaneously active, each with a
 * different beacon, and the whole picture shifts every 10 seconds.
 *****************************************************************************/
#pragma once

#include <lvgl.h>
#include <time.h>

#define NUM_BEACONS 18

/** One entry per beacon, in official NCDXF transmission-order (BEACONS[0]
 *  is the one that starts the 14.100 MHz rotation at slot 0). */
typedef struct {
    const char *callsign;
    const char *location;
    float lat;
    float lon;
} beacon_t;

extern const beacon_t BEACONS[NUM_BEACONS];

/**
 * Creates the beacon panel (as a child of `parent`, normally
 * lv_scr_act()) at (x, y), width `w`, height `h` -- mirrors
 * solar_conditions.cpp's column: same highlight-box row style,
 * vertically centered content. Placeholder "--" rows until the first
 * beacon_panel_update() call.
 */
void beacon_panel_init(lv_obj_t *parent, int x, int y, int w, int h);

/** Recomputes which beacon is on which band for `utc_now` and updates
 *  the rows. Cheap (array indexing + snprintf), fine to call every
 *  second even though the underlying schedule only actually changes
 *  every 10 seconds. */
void beacon_panel_update(time_t utc_now);
