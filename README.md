# AmateurRadioClock (ESP-IDF)

A day/night world clock for the Waveshare ESP32-S3-Touch-LCD-7B (1024x600 RGB panel), built for ham radio operators. Runs natively on ESP-IDF rather than the Arduino framework -- the framework's scheduling overhead was found to eat into the RGB panel's scan-out timing margin badly enough to cause visible display jitter that no pixel-clock tuning could fully resolve. Removing that overhead (native ESP-IDF, no Arduino) fixed it.

## Features

- Live day/night terminator overlaid on a world map, redrawn once a minute
- Local + UTC time in the top bar, synced via NTP
- Temperature/humidity from a DHT11 sensor, read via the RMT peripheral (hardware-timestamped, immune to WiFi-induced timing jitter -- a CPU-polling implementation was tried first and proved unreliable under real WiFi traffic)
- Five-city clock bar with sunrise/sunset times
- HF band-conditions panel (solar flux index, sunspot number, A/K index, day/night propagation quality per band), fetched from hamqsl.com
- NCDXF/IARU International Beacon Project panel: which beacon is transmitting on each of the 5 HF beacon frequencies right now, computed purely from the clock's own accurate time (no network needed) -- plus a map marker at each of the 18 beacon locations, highlighting whichever ones are currently active
- WiFi status indicator

## Hardware

- Waveshare ESP32-S3-Touch-LCD-7B, 1024x600 RGB parallel LCD (this build targets the non-touch variant -- touch init is treated as non-fatal if the panel isn't present)
- DHT11 temperature/humidity sensor on the board's labeled 3-pin GPIO header (3V3/GND/GPIO6)
- microSD card (FAT32) holding `worldmap.bin` -- a raw RGB565 1024x600 image, no header, little-endian (see `tools/make_worldmap.py` in the Arduino-era sibling project for how this is generated)

## Building

Requires ESP-IDF v5.5+ (the managed `esp_lvgl_adapter` component depends on it).

```
idf.py set-target esp32s3
idf.py -p PORT flash monitor
```

`sdkconfig.defaults` carries the project's actual configuration (PSRAM layout, partition table, WiFi/TLS buffer sizes, LVGL memory pool, etc.) -- if you change it, delete the generated `sdkconfig` and rebuild so the change actually takes effect.

Per-device settings (WiFi credentials, callsign, timezone, city list) live in `main/config.h`.

## Project layout

- `main/main.cpp` -- boot sequence, top bar, UI layout
- `main/daynight_map.cpp` -- the map canvas and terminator recompute
- `main/city_clocks.cpp` -- bottom bar city clocks + sunrise/sunset
- `main/sun_position.cpp` -- solar position math (NOAA algorithm)
- `main/solar_conditions.cpp` -- HF band-conditions HTTPS fetch + panel
- `main/beacon_panel.cpp` -- NCDXF beacon schedule panel + map markers
- `main/env_sensor.cpp` -- DHT11 read (RMT-based)
- `main/wifi_manager.cpp` -- native `esp_wifi` station connect + NTP sync
- `components/` -- vendor board-support components (RGB panel init, SD card, IO expander)

## Acknowledgements

Ported from Waveshare's ESP-IDF LVGL demo for this board. Beacon schedule timing verified against an existing open-source ham-radio clock project's implementation of the same NCDXF schedule: [SmittyHalibut/HamClock](https://github.com/SmittyHalibut/HamClock).

This project is not affiliated with, endorsed by, or associated with Geochron, Geochronmap, or HamClock -- it is an independent build inspired by the general concept those projects/products popularized (a world map with a live day/night terminator for ham radio use). Any resemblance in naming has been intentionally avoided; Geochron and Geochronmap in particular are trademarks of their respective owners.
