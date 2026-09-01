/*****************************************************************************
 * env_sensor.h
 *
 * Temperature/humidity from a DHT11 wired to the board's labeled 3-pin
 * GPIO expansion header (3V3 / GND / GP6 -- GPIO6). The Arduino build
 * used the Adafruit DHT sensor library; this is a from-scratch
 * reimplementation of the same bit-banged single-wire protocol directly
 * against ESP-IDF's GPIO driver (native `driver/gpio.h` + `esp_rom_delay_us`
 * for microsecond timing), since that Arduino library isn't available
 * here.
 *
 * The read (a ~5ms interrupt-disabling critical section -- the protocol's
 * timing is tight enough that FreeRTOS task scheduling would otherwise
 * corrupt it) runs on a dedicated core-0 FreeRTOS task, started
 * internally by env_sensor_init(). On the Arduino build, running this
 * from the main loop on the same core as LVGL rendering and the RGB
 * panel's vsync-completion signal caused display jitter; kept here as
 * sound practice even though that specific mechanism turned out not to
 * be this project's core jitter problem after all (see daynight_map.h).
 *****************************************************************************/
#pragma once

#include <lvgl.h>

/** Creates the temperature/humidity label as a child of `parent` at (x, y),
 *  and starts the dedicated core-0 task that reads and updates it. */
void env_sensor_init(lv_obj_t *parent, int x, int y);
