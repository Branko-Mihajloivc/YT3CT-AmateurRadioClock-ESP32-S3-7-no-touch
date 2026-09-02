/*****************************************************************************
 * range_rings.h
 *
 * Great-circle distance rings centered on Knjazevac (the home QTH), drawn
 * on the day/night map, plus small markers at the 8 main compass points on
 * each ring. On this map's equirectangular projection a true circle of
 * constant distance from a point NOT on the equator does not look like a
 * circle -- it's a distorted, ellipse-like curve that pinches tighter the
 * closer the ring's radius gets to the distance from the QTH to the pole.
 * That's expected, not a bug -- each ring is built from real great-circle
 * "destination point given start, bearing, distance" math (spherical
 * Earth), swept around 360 degrees of bearing, not a naive drawn ellipse.
 *****************************************************************************/
#pragma once

#include <lvgl.h>

/**
 * Creates the range rings as a child of `parent` (the map canvas's
 * parent, same as daynight_map_init/beacon_panel_init). Static content --
 * Knjazevac's position and the ring radii never change at runtime -- so
 * this only needs to run once, unlike the map's own day/night shading.
 */
void range_rings_init(lv_obj_t *parent);
