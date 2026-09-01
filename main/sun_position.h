/*****************************************************************************
 * sun_position.h
 *
 * Minimal solar-position calculator: given a UTC date/time, returns the
 * subsolar point -- the single point on Earth where the sun is directly
 * overhead. That point's latitude/longitude is all that's needed to draw
 * a day/night terminator on a world map, because the terminator is simply
 * the circle 90 degrees away (in great-circle terms) from the subsolar
 * point.
 *
 * This is the standard NOAA simplified solar-position algorithm (the same
 * one behind the NOAA Solar Calculator spreadsheet and most "sunrise/
 * sunset"/gray-line Arduino sketches). Accuracy is roughly +/-0.01 degree,
 * far tighter than a 1024x600 display can show, and it needs nothing but
 * the current UTC time -- no network call, no ephemeris table.
 *****************************************************************************/
#pragma once

#include <time.h>

/**
 * @param utc        Broken-down UTC time (as from gmtime_r()).
 * @param lat_deg    [out] Subsolar latitude in degrees (== solar declination).
 * @param lon_deg    [out] Subsolar longitude in degrees, normalized to [-180, 180).
 */
void sun_subsolar_point(const struct tm *utc, double *lat_deg, double *lon_deg);

/**
 * Sunrise/sunset for a given place and date, using the same declination and
 * equation-of-time math as sun_subsolar_point() (standard -0.833 degree
 * refraction+disk-radius convention -- not the -6 degree civil-twilight
 * threshold daynight_map.cpp uses for the terminator band).
 *
 * @param utc                UTC date/time (only the date part matters, but
 *                            pass the current time as from gmtime_r()).
 * @param lat_deg, lon_deg   Location.
 * @param sunrise_utc_hours  [out] Sunrise time, UTC hours (0..24).
 * @param sunset_utc_hours   [out] Sunset time, UTC hours (0..24).
 *                            Both are set to NAN if the sun doesn't rise or
 *                            set at all on this date/latitude (polar
 *                            day/night) -- doesn't happen at any of this
 *                            project's city latitudes, but handled cleanly.
 */
void sun_rise_set(const struct tm *utc, double lat_deg, double lon_deg,
                   double *sunrise_utc_hours, double *sunset_utc_hours);
