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

// Precomputed per-row/per-column trig for compute_terminator()'s inner
// loop (a handful of multiply-adds instead of calling sin/cos 614,400
// times). Used to be plain `static float array[N]` -- harmless on its own,
// but combined with this task's own internal-RAM stack (see
// xTaskCreatePinnedToCore below) it was part of what pushed internal RAM
// low enough to make solar_conditions' HTTPS fetch start silently timing
// out (see the isolation test in project memory/commit history). Moved to
// PSRAM like the map's pixel buffers already were.
static float *s_sinLatRow = NULL;
static float *s_cosLatRow = NULL;
static float *s_cosLonCol = NULL;
static float *s_sinLonCol = NULL;

static void map_task(void *arg); // defined below daynight_map_init(), which starts it

// map_task's own stack, PSRAM-backed for the same reason as the trig
// tables above -- see the comment there.
static const size_t MAP_TASK_STACK_BYTES = 8192;
static StackType_t *s_map_task_stack = NULL;
static StaticTask_t s_map_task_tcb;

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
    s_sinLatRow = (float *)heap_caps_malloc((size_t)H * sizeof(float), MALLOC_CAP_SPIRAM);
    s_cosLatRow = (float *)heap_caps_malloc((size_t)H * sizeof(float), MALLOC_CAP_SPIRAM);
    s_cosLonCol = (float *)heap_caps_malloc((size_t)W * sizeof(float), MALLOC_CAP_SPIRAM);
    s_sinLonCol = (float *)heap_caps_malloc((size_t)W * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_base_buf || !s_work_buf || !s_sinLatRow || !s_cosLatRow || !s_cosLonCol || !s_sinLonCol) {
        printf("daynight_map: PSRAM allocation failed\n");
        return false;
    }
    for (int y2 = 0; y2 < H; y2++) {
        float lat = deg2radf(90.0f - ((float)y2 + 0.5f) / H * 180.0f);
        s_sinLatRow[y2] = sinf(lat);
        s_cosLatRow[y2] = cosf(lat);
    }
    for (int x2 = 0; x2 < W; x2++) {
        float lon = deg2radf(((float)x2 + 0.5f) / W * 360.0f - 180.0f);
        s_cosLonCol[x2] = cosf(lon);
        s_sinLonCol[x2] = sinf(lon);
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

    s_map_task_stack = (StackType_t *)heap_caps_malloc(MAP_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM);
    if (s_map_task_stack) {
        xTaskCreateStaticPinnedToCore(map_task, "daynight_map", MAP_TASK_STACK_BYTES, NULL, 1,
                                       s_map_task_stack, &s_map_task_tcb, MAP_TASK_CORE);
    } else {
        printf("daynight_map: PSRAM stack allocation failed, map won't update\n");
    }

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

    // Per-row/per-column trig, precomputed once in daynight_map_init()
    // into s_sinLatRow/s_cosLatRow/s_cosLonCol/s_sinLonCol (PSRAM) --
    // avoids calling sin/cos 614,400 times per recompute.

    const float cosLon0 = cosf(lon0);
    const float sinLon0 = sinf(lon0);
    const float denom = (COSZ_DAY_EDGE - COSZ_NIGHT_EDGE);

    for (int y = 0; y < H; y++) {
        const float sLat = s_sinLatRow[y];
        const float cLat = s_cosLatRow[y];
        const uint16_t *baseRow = s_base_buf + (size_t)y * W;
        uint16_t *workRow = s_work_buf + (size_t)y * W;

        for (int x = 0; x < W; x++) {
            float cosDeltaLon = s_cosLonCol[x] * cosLon0 + s_sinLonCol[x] * sinLon0;
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
