/*****************************************************************************
 * daynight_map.h
 *
 * Owns the LVGL canvas that shows the world map with a live day/night
 * terminator overlay. Fills the full screen -- the top bar,
 * band-conditions column, and city-clocks bar float as text directly on
 * top of it (see main.cpp), with no background of their own. Two RGB565
 * framebuffers live in PSRAM: `base_buf` is the pristine map (loaded once
 * from the SD card), `work_buf` is what the canvas actually shows -- it's
 * re-derived from base_buf on every terminator update by darkening
 * whichever half of the globe is currently in night, written directly in
 * place (see compute_terminator() in the .cpp for why that's safe without
 * an intermediate staging buffer).
 *
 * The recompute (and the sunrise/sunset update it's paired with) runs on
 * its own dedicated core-0 FreeRTOS task, started internally by
 * daynight_map_init(). This is ported from the original Arduino-framework
 * build of this project, where a jitter/ghosting glitch on this exact
 * board was eventually root-caused to the RGB panel's pixel clock not
 * having enough scan-out timing margin (see rgb_lcd_port.h) -- an
 * extensive elimination test ruled out every periodic application task
 * (including this one) before landing there. The port to ESP-IDF was
 * motivated by that finding: Waveshare's own unmodified ESP-IDF LVGL demo
 * ran clean at a pixel clock nearly 50% higher than anything achievable
 * on the Arduino build, meaning removing Arduino's framework overhead
 * recovers real scan-out margin. This core-0 task placement is kept
 * anyway as sound practice, not because it was the fix.
 * Nothing else needs to call anything here beyond daynight_map_init().
 *****************************************************************************/
#pragma once

#include <lvgl.h>
#include <stdbool.h>

#define DAYNIGHT_MAP_W 1024
#define DAYNIGHT_MAP_H 600

/**
 * Creates the canvas as a child of `parent`, positioned at (x, y),
 * attempts to load /sdcard/worldmap.bin (raw RGB565, DAYNIGHT_MAP_W x
 * DAYNIGHT_MAP_H, little-endian, no header -- see tools/make_worldmap.py)
 * into it, and starts the dedicated core-0 task that keeps the terminator
 * (and each city's sunrise/sunset, see city_clocks.h) up to date on its
 * own once-a-minute cadence. The first frame shows the base map untouched
 * (full "day") until that task's first cycle fills in the real
 * terminator a few seconds later.
 *
 * @param parent        LVGL parent object (normally lv_scr_act()).
 * @param x, y          Top-left position of the map canvas within parent.
 * @param sd_available  Pass true only if sd_mmc_init() already succeeded.
 * @return true if the map image was found and loaded; false if a
 *         placeholder background was used instead (check this and warn
 *         the user).
 */
bool daynight_map_init(lv_obj_t *parent, int x, int y, bool sd_available);
