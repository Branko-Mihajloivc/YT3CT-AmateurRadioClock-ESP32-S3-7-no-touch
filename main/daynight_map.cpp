#include "daynight_map.h"
#include "sd.h"
#include "sun_position.h"
#include "city_clocks.h"
#include "esp_lv_adapter.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int W = DAYNIGHT_MAP_W;
static const int H = DAYNIGHT_MAP_H;

static inline float deg2radf(float d) { return d * (float)M_PI / 180.0f; }

static lv_obj_t *s_canvas = NULL;
static uint16_t *s_base_buf = NULL; // pristine map, loaded once
static uint16_t *s_work_buf = NULL; // what's actually displayed (bound to the LVGL canvas)

static void map_task(void *arg); // defined below daynight_map_init(), which starts it

// Stagger vs env_sensor.h's DHT task (10s first read, 30s cadence) and
// solar_conditions.h's fetch task (15s first fetch, 3600s cadence) -- all
// three share core 0. 60000/30000 aren't coprime, so a fixed initial
// offset stays a fixed gap forever rather than drifting: 20s keeps this
// permanently ~10s clear of every DHT firing and ~5s clear of the (rare)
// solar fetch.
static const uint32_t FIRST_UPDATE_DELAY_MS = 20UL * 1000UL;
static const uint32_t MAP_UPDATE_INTERVAL_MS = 60UL * 1000UL; // terminator moves slowly; once a minute is plenty
static const int MAP_TASK_CORE = 0; // NOT core 1 -- see daynight_map.h and env_sensor.h

// cos(zenith angle) thresholds: 0 = geometric sunset/sunrise,
// -0.1045 ~= -6 degrees solar elevation (civil twilight). Between the two
// we blend linearly for a soft terminator band instead of a hard edge.
static const float COSZ_DAY_EDGE = 0.0f;
static const float COSZ_NIGHT_EDGE = -0.1045f;

static inline void unpack565(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (px >> 11) & 0x1F;
    *g = (px >> 5) & 0x3F;
    *b = px & 0x1F;
}

static inline uint16_t pack565(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 63) g = 63;
    if (b < 0) b = 0; else if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void fill_placeholder(uint16_t *buf) {
    // Flat dark-ocean color so the app still runs (and the missing-map
    // warning label is legible) even without the SD asset in place.
    uint16_t c = pack565(3, 8, 14);
    for (int i = 0; i < W * H; i++) buf[i] = c;
}

static bool load_base_map_from_sd() {
    FILE *f = fopen(MOUNT_POINT "/worldmap.bin", "rb");
    if (!f) {
        printf("daynight_map: could not open %s/worldmap.bin\n", MOUNT_POINT);
        return false;
    }
    size_t want = (size_t)W * H * 2;
    size_t got = fread(s_base_buf, 1, want, f);
    fclose(f);
    if (got != want) {
        printf("daynight_map: worldmap.bin is %u bytes, expected %u\n",
               (unsigned)got, (unsigned)want);
        return false;
    }
    return true;
}

bool daynight_map_init(lv_obj_t *parent, int x, int y, bool sd_available) {
    s_base_buf = (uint16_t *)heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM);
    s_work_buf = (uint16_t *)heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM);
    if (!s_base_buf || !s_work_buf) {
        printf("daynight_map: PSRAM allocation failed\n");
        return false;
    }

    bool ok = false;
    if (sd_available) {
        ok = load_base_map_from_sd();
    }
    if (!ok) {
        fill_placeholder(s_base_buf);
    }

    // First frame: just show the base map untouched (full "day") until
    // map_task's first cycle fills in the real terminator a few seconds
    // after boot.
    memcpy(s_work_buf, s_base_buf, (size_t)W * H * 2);

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_work_buf, W, H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_canvas, x, y);

    xTaskCreatePinnedToCore(map_task, "daynight_map", 8192, NULL, 1, NULL, MAP_TASK_CORE);

    return ok;
}

// Draws a small filled circle (the "sun now" marker) directly into `buf`.
static void draw_sun_marker(uint16_t *buf, int cx, int cy) {
    const int r = 6;
    const uint16_t color = pack565(31, 40, 0); // amber "sun now" marker
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= H) continue;
        int dx_max = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        for (int dx = -dx_max; dx <= dx_max; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= W) continue;
            s_work_buf[y * W + x] = color;
        }
    }
}

// Heavy per-pixel recompute -- writes directly into the live canvas
// buffer. Safe to do without holding esp_lv_adapter_lock: LVGL's canvas
// widget only ever reads this buffer during a redraw pass triggered by an
// explicit lv_obj_invalidate() call, and map_task defers that call until
// this entire function has finished writing every pixel -- so there's no
// window where the render task could see a half-updated frame.
static void compute_terminator(double subsolar_lat_deg, double subsolar_lon_deg) {
    if (!s_base_buf || !s_work_buf) return;

    const float lat0 = deg2radf((float)subsolar_lat_deg);
    const float lon0 = deg2radf((float)subsolar_lon_deg);
    const float sinLat0 = sinf(lat0);
    const float cosLat0 = cosf(lat0);

    // Precompute per-row / per-column trig so the inner loop is a handful
    // of multiply-adds instead of calling sin/cos 614,400 times.
    static float sinLatRow[DAYNIGHT_MAP_H];
    static float cosLatRow[DAYNIGHT_MAP_H];
    static float cosLonCol[DAYNIGHT_MAP_W];
    static float sinLonCol[DAYNIGHT_MAP_W];
    static bool trig_tables_built = false;

    if (!trig_tables_built) {
        for (int y = 0; y < H; y++) {
            float lat = deg2radf(90.0f - ((float)y + 0.5f) / H * 180.0f);
            sinLatRow[y] = sinf(lat);
            cosLatRow[y] = cosf(lat);
        }
        for (int x = 0; x < W; x++) {
            float lon = deg2radf(((float)x + 0.5f) / W * 360.0f - 180.0f);
            cosLonCol[x] = cosf(lon);
            sinLonCol[x] = sinf(lon);
        }
        trig_tables_built = true;
    }

    const float cosLon0 = cosf(lon0);
    const float sinLon0 = sinf(lon0);
    const float denom = (COSZ_DAY_EDGE - COSZ_NIGHT_EDGE);

    for (int y = 0; y < H; y++) {
        const float sLat = sinLatRow[y];
        const float cLat = cosLatRow[y];
        const uint16_t *baseRow = s_base_buf + (size_t)y * W;
        uint16_t *workRow = s_work_buf + (size_t)y * W;

        for (int x = 0; x < W; x++) {
            float cosDeltaLon = cosLonCol[x] * cosLon0 + sinLonCol[x] * sinLon0;
            float cosz = sLat * sinLat0 + cLat * cosLat0 * cosDeltaLon;

            uint16_t px = baseRow[x];
            if (cosz >= COSZ_DAY_EDGE) {
                workRow[x] = px; // full daylight, unmodified
                continue;
            }

            uint8_t r, g, b;
            unpack565(px, &r, &g, &b);

            // Night tone: strongly dim red/green, keep a little blue so
            // ocean/land don't crush to pure black.
            float rN = r * 0.20f;
            float gN = g * 0.24f;
            float bN = b * 0.55f + 2.0f;

            if (cosz <= COSZ_NIGHT_EDGE) {
                workRow[x] = pack565((int)rN, (int)gN, (int)bN);
            } else {
                float t = (COSZ_DAY_EDGE - cosz) / denom; // 0..1 across the twilight band
                float rB = r + (rN - r) * t;
                float gB = g + (gN - g) * t;
                float bB = b + (bN - b) * t;
                workRow[x] = pack565((int)rB, (int)gB, (int)bB);
            }
        }

        // Brief yield every 16 rows -- on the Arduino build this loop's
        // ~614k PSRAM reads/writes competed with the RGB panel's own
        // PSRAM-backed DMA closely enough to cause visible jitter, tied
        // precisely to this update's once-a-minute cadence. Kept here
        // as a cheap precaution even though the real fix turned out to be
        // the pixel clock margin (see rgb_lcd_port.h) -- yielding
        // periodically during a large PSRAM-heavy loop remains good
        // hygiene regardless of how much margin the platform has.
        if ((y % 16) == 15) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    // subsolar ("sun now") marker
    int sx = (int)((subsolar_lon_deg + 180.0) / 360.0 * W);
    int sy = (int)((90.0 - subsolar_lat_deg) / 180.0 * H);
    draw_sun_marker(s_work_buf, sx, sy);
}

static void map_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(FIRST_UPDATE_DELAY_MS));
    while (true) {
        time_t now = time(nullptr);
        struct tm utc_tm;
        gmtime_r(&now, &utc_tm);

        double lat0, lon0;
        sun_subsolar_point(&utc_tm, &lat0, &lon0);
        compute_terminator(lat0, lon0); // heavy, no lock held -- see the comment on compute_terminator() for why this is safe

        if (esp_lv_adapter_lock(200) == ESP_OK) {
            lv_obj_invalidate(s_canvas);
            esp_lv_adapter_unlock();
        }
        city_clocks_update_sun_times(&utc_tm); // same once-a-minute cadence as the terminator; own brief lock use internally

        // Direct-mode's double-buffer ping-pong only paints a given
        // redraw into whichever physical buffer is active at that moment
        // -- the invalidate() call above only reaches one of the two
        // buffers, leaving the other stuck showing the previous
        // terminator position. Two more full-screen redraws here (lock
        // released between them so the render task actually gets to flip
        // in between) get both buffers to agree on this update's real
        // content.
        for (int i = 0; i < 2; i++) {
            if (esp_lv_adapter_lock(100) == ESP_OK) {
                lv_obj_invalidate(lv_scr_act());
                esp_lv_adapter_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        vTaskDelay(pdMS_TO_TICKS(MAP_UPDATE_INTERVAL_MS));
    }
}
