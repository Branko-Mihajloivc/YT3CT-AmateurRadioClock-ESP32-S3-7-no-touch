#include "sun_position.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline double deg2rad(double d) { return d * M_PI / 180.0; }
static inline double rad2deg(double r) { return r * 180.0 / M_PI; }

// Julian Day (including time-of-day fraction) from a UTC calendar date/time.
static double julian_day(const struct tm *utc) {
    int year = utc->tm_year + 1900;
    int month = utc->tm_mon + 1;
    int day = utc->tm_mday;
    double hour = utc->tm_hour + utc->tm_min / 60.0 + utc->tm_sec / 3600.0;

    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    int A = year / 100;
    int B = 2 - A + A / 4;

    double jd = floor(365.25 * (year + 4716))
              + floor(30.6001 * (month + 1))
              + day + B - 1524.5 + hour / 24.0;
    return jd;
}

// Solar declination and equation of time for a given UTC date/time -- the
// two quantities both sun_subsolar_point() and sun_rise_set() are built on,
// factored out here so the orbital-mechanics math exists in exactly one
// place.
static void sun_decl_eqtime(const struct tm *utc, double *decl_deg, double *eqtime_min) {
    double jd = julian_day(utc);
    double T = (jd - 2451545.0) / 36525.0; // Julian centuries since J2000.0

    // Geometric mean longitude and anomaly of the sun (degrees)
    double L0 = fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
    if (L0 < 0) L0 += 360.0;
    double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    double Mr = deg2rad(M);

    double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T); // eccentricity

    // Equation of center
    double C = sin(Mr) * (1.914602 - T * (0.004817 + 0.000014 * T))
             + sin(2 * Mr) * (0.019993 - 0.000101 * T)
             + sin(3 * Mr) * 0.000289;

    double true_long = L0 + C; // true longitude of the sun (degrees)

    // Apparent longitude, correcting for nutation/aberration
    double omega = 125.04 - 1934.136 * T;
    double lambda = true_long - 0.00569 - 0.00478 * sin(deg2rad(omega));

    // Mean and corrected obliquity of the ecliptic (degrees)
    double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    double eps_corr = eps0 + 0.00256 * cos(deg2rad(omega));

    // Solar declination
    double decl = asin(sin(deg2rad(eps_corr)) * sin(deg2rad(lambda)));
    *decl_deg = rad2deg(decl);

    // Equation of time (minutes): difference between apparent and mean solar time
    double y = tan(deg2rad(eps_corr / 2.0));
    y *= y;
    *eqtime_min = 4.0 * rad2deg(
        y * sin(2 * deg2rad(L0))
        - 2 * e * sin(Mr)
        + 4 * e * y * sin(Mr) * cos(2 * deg2rad(L0))
        - 0.5 * y * y * sin(4 * deg2rad(L0))
        - 1.25 * e * e * sin(2 * Mr)
    );
}

void sun_subsolar_point(const struct tm *utc, double *lat_deg, double *lon_deg) {
    double decl_deg, eqtime_min;
    sun_decl_eqtime(utc, &decl_deg, &eqtime_min);
    *lat_deg = decl_deg; // subsolar latitude == solar declination

    // Subsolar longitude: the meridian currently experiencing solar noon.
    // Local apparent solar time = UTC + lon/15 + eqtime/60 = 12 at the
    // subsolar point, which rearranges to the line below (see README.md
    // for the derivation).
    double utc_hours = utc->tm_hour + utc->tm_min / 60.0 + utc->tm_sec / 3600.0;
    double lon0 = 180.0 - 15.0 * utc_hours - 0.25 * eqtime_min;

    // normalize to [-180, 180)
    lon0 = fmod(lon0 + 180.0, 360.0);
    if (lon0 < 0) lon0 += 360.0;
    lon0 -= 180.0;

    *lon_deg = lon0;
}

void sun_rise_set(const struct tm *utc, double lat_deg, double lon_deg,
                   double *sunrise_utc_hours, double *sunset_utc_hours) {
    double decl_deg, eqtime_min;
    sun_decl_eqtime(utc, &decl_deg, &eqtime_min);

    double lat_r = deg2rad(lat_deg);
    double decl_r = deg2rad(decl_deg);

    // Hour angle at sunrise/sunset: -0.833 degrees is the standard
    // refraction + solar-disk-radius convention (distinct from the -6
    // degree civil-twilight threshold daynight_map.cpp uses for the
    // terminator band).
    double cosH = (sin(deg2rad(-0.833)) - sin(lat_r) * sin(decl_r)) / (cos(lat_r) * cos(decl_r));

    if (cosH < -1.0 || cosH > 1.0) {
        // Polar day (sun never sets) or polar night (sun never rises) --
        // doesn't happen at this project's city latitudes, but handled
        // cleanly rather than feeding acos() an out-of-domain value.
        *sunrise_utc_hours = NAN;
        *sunset_utc_hours = NAN;
        return;
    }

    // Solar noon (UTC hours) at this longitude -- same relation as in
    // sun_subsolar_point(), solved for utc_hours given a fixed longitude
    // instead of solving for longitude given utc_hours.
    double solar_noon_utc = 12.0 - lon_deg / 15.0 - eqtime_min / 60.0;

    double half_day_hours = rad2deg(acos(cosH)) / 15.0;

    // fmod() twice (rather than a single "+ 24" before the mod) so this
    // stays correct even for the theoretical extreme-longitude/near-polar
    // inputs this function's callers don't currently use.
    double sunrise = fmod(fmod(solar_noon_utc - half_day_hours, 24.0) + 24.0, 24.0);
    double sunset = fmod(fmod(solar_noon_utc + half_day_hours, 24.0) + 24.0, 24.0);

    *sunrise_utc_hours = sunrise;
    *sunset_utc_hours = sunset;
}
