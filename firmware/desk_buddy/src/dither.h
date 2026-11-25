// Smooth gradients on the RGB565 panel. 565 has 32 levels of red/blue and 64 of green, so a
// soft gradient on a dark background shows rings. An ordered (Bayer 8x8) dither spreads each
// step over neighboring pixels, which the eye averages back into a smooth gradient.
// Used by the backgrounds painted pixel by pixel (Home's sun ring, Weather's glow).
#pragma once

#include <math.h>
#include <stdint.h>

namespace dither {

struct RGB { float r, g, b; };

inline RGB rgb(uint32_t hex) { return {float((hex >> 16) & 0xFF), float((hex >> 8) & 0xFF), float(hex & 0xFF)}; }
inline RGB mix(const RGB &a, const RGB &b, float t) {  // a -> b
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

inline const uint8_t BAYER8[8][8] = {
  {0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
  {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
  {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
  {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

inline uint16_t to565(const RGB &c, int x, int y) {
  float t = (BAYER8[y & 7][x & 7] + 0.5f) / 64.0f;  // 0..1
  auto q = [t](float v, float levels) {              // v 0..255 -> 0..levels-1
    float s = v / 255.0f * (levels - 1) + t;
    return (uint16_t)fmaxf(0, fminf(levels - 1, floorf(s)));
  };
  return (uint16_t)((q(c.r, 32) << 11) | (q(c.g, 64) << 5) | q(c.b, 32));
}

}  // namespace dither
