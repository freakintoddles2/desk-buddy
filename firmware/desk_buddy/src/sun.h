// Sunrise and sunset, shared by Home's sky, the moon and auto brightness.
#pragma once

#include <math.h>
#include <time.h>

// rise/set in local hours (may fall outside 0-24 near the poles). polar: 0 normal,
// +1 the sun never sets today (midnight sun), -1 it never rises (polar night).
struct SunTimes {
  float rise, set;
  int polar = 0;
};

// Standard NOAA-style approximation, same as display.html's sunTimes(). A few minutes off at
// mid latitudes, more near the poles.
inline SunTimes sunTimes(const struct tm &d, float tzH, float lat, float lon) {
  const float rad = M_PI / 180.0f;
  int doy = d.tm_yday + 1;
  float B = (2 * M_PI / 365.0f) * (doy - 81);
  float decl = 23.44f * sinf(B);
  float eot = 9.87f * sinf(2 * B) - 7.53f * cosf(B) - 1.5f * sinf(B);  // minutes
  float noon = 12 + (tzH * 15 - lon) / 15 - eot / 60;
  float cosH = (sinf(-0.833f * rad) - sinf(lat * rad) * sinf(decl * rad)) /
               (cosf(lat * rad) * cosf(decl * rad));
  float half = acosf(fmaxf(-1, fminf(1, cosH))) / rad / 15;
  return {noon - half, noon + half, cosH <= -1 ? 1 : cosH >= 1 ? -1 : 0};
}
