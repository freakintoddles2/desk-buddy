// Living sky's weather: clouds, rain, snow, fog and storms painted over the sky for the current
// conditions (mockups/weather-sky-mockups.html; display.html paintWeatherSky mirrors this file).
//
// One painter covers every state. Each Open-Meteo weather (WMO) code sets a few numbers in a
// table (cloud cover and darkness, how gray the sky goes, how much of the sun shows, rain or
// snow and how much, fog, lightning), so nothing is stored per state. Clouds come from a 4 KB
// noise table in PSRAM; rain, snow, splashes and drips on the glass from integer hashes.
#pragma once

#include <functional>
#include <stdint.h>

#include "dither.h"

namespace skywx {

// WMO code to use with no weather data: Mainly clear (2026-11-26).
constexpr int NO_DATA = 1;

// Everything the painter needs for one minute; home.cpp fills it from its Scene.
struct Frame {
  dither::RGB top, bot;  // the clear sky's top and horizon colors now (0..255)
  float hour = 12, rise = 6, set = 18;
  bool night = false;
  float bx = 240, by = 240;  // the sun or moon, 480 px coordinates
  float sunR = 22, moonR = 20.4f;
  float glowReach = 0.30f, glowDay = 0.35f, glowNight = 0.025f, glowNightLit = 0.075f;
  uint32_t glowTint = 0xFFF1C8;
  float moonAge = 0;
  bool south = false;
  int code = NO_DATA;  // WMO weather code
  int drift = 0;       // minutes: clouds drift and rain and snow move between paints
};

// Clouds hide the sun or moon for this code: the painter draws it (dimmed) under the clouds
// and Home hides its sprite. Otherwise the sprite stays and the painter leaves the body out.
bool coversBody(int code);

// Paint an n x n RGB565 buffer (n = 480 for Home, smaller for the customize previews).
// body: paint the sun or moon in (previews, or when coversBody). stop is checked every 8 rows;
// true abandons the paint (returns false). Yields every 8 rows (core 0's watchdog).
bool paint(uint16_t *buf, int n, const Frame &f, bool body, const std::function<bool()> &stop);

}  // namespace skywx
