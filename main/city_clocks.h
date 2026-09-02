/*****************************************************************************
 * city_clocks.h
 *
 * A small fixed row of DX-relevant city clocks: local time plus that
 * city's own sunrise/sunset, each rendered in the city's own local time
 * (not UTC). Knjazevac (the home QTH) is included unconditionally,
 * independent of wherever config.h's LOCAL_TZ says this particular board
 * is physically deployed.
 *
 * The city table (name, POSIX TZ string, lat/lon) is a small static array
 * at the top of city_clocks.cpp -- easy to edit if you want different
 * cities.
 *****************************************************************************/
#pragma once

#include <lvgl.h>
#include <time.h>

/**
 * Creates the bottom city-clocks bar as a child of `parent`, spanning
 * (x, y) to (x+w, y+h), with one city per equal-width column.
 */
void city_clocks_init(lv_obj_t *parent, int x, int y, int w, int h);

/**
 * Updates each city's local HH:MM. Cheap (just timezone conversions), fine
 * to call every second alongside the UTC/local clock labels.
 */
void city_clocks_tick(time_t now);

/**
 * Recomputes each city's sunrise/sunset (in that city's own local time)
 * for the given UTC date. Call this on the same once-a-minute cadence as
 * the day/night map update -- sunrise/sunset times shift far too slowly to
 * need any faster.
 */
void city_clocks_update_sun_times(const struct tm *utc);

/**
 * Screen x-coordinate of the Knjazevac column's center, as actually laid
 * out by city_clocks_init() (equal-width columns by index, not projected
 * from its real lat/lon -- the two don't match). Only valid after
 * city_clocks_init() has run. Lets other UI (e.g. the callsign) align to
 * where "Knjazevac" is really drawn on screen instead of duplicating this
 * module's column-layout math.
 */
int city_clocks_get_knjazevac_x(void);
