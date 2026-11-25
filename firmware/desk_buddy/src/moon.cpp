#include "moon.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "tz.h"
#include "weather.h"

namespace moon {

namespace {

using dither::RGB;
using dither::rgb;
using dither::mix;

constexpr double SYNODIC = 29.530588853;  // days
constexpr time_t NEW_2000 = 947182440;    // 2000-01-06 18:14 UTC

// Maria as {x, y, rx, ry} in moon radii, north up as seen from the north: Imbrium,
// Procellarum, Serenitatis, Tranquillitatis, Crisium, Nubium, Fecunditatis.
constexpr float MARIA[7][4] = {{-.32f, -.38f, .3f, .27f}, {-.6f, .05f, .29f, .46f}, {.18f, -.33f, .19f, .18f},
                               {.3f, -.02f, .22f, .2f},   {.66f, -.22f, .13f, .15f}, {-.12f, .42f, .21f, .16f},
                               {.5f, .25f, .14f, .18f}};

// A mare's strength by distance from its center in its own radii (the faux's gradient stops).
float mareA(float q) {
  if (q >= 1) return 0;
  if (q < .35f) return .26f + (.2f - .26f) * (q / .35f);
  if (q < .7f) return .2f + (.08f - .2f) * ((q - .35f) / .35f);
  return .08f * (1 - (q - .7f) / .3f);
}

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Straight-alpha "over".
void over(Px &d, const RGB &c, float a) {
  if (a <= 0) return;
  float out = a + d.a * (1 - a);
  if (out <= 0) return;
  float k = d.a * (1 - a);
  d.c = {(c.r * a + d.c.r * k) / out, (c.g * a + d.c.g * k) / out, (c.b * a + d.c.b * k) / out};
  d.a = out;
}

}  // namespace

float age(time_t utc) {
  double days = (double)(utc - NEW_2000) / 86400.0;
  double f = fmod(days, SYNODIC);
  if (f < 0) f += SYNODIC;
  return (float)(f / SYNODIC);
}

float lit(float a) { return (1 - cosf(2 * (float)M_PI * a)) / 2; }

bool south() {
  float lat, lon;
  if (!weather::coords(lat, lon)) tz::fallbackCoords(lat, lon);
  return lat < 0;
}

Px pixel(float u, float v, float aa, float a, bool s) {
  Px p{{0, 0, 0}, 0};
  const float d = sqrtf(u * u + v * v);
  if (d > 1.04f + aa) return p;
  const float disc = clamp01((1 - d) / aa + .5f);
  // The dark part, faintly, and its outline (stroke .08 r centered on the edge).
  over(p, rgb(0xBEC8EB), .13f * disc);
  over(p, rgb(0xD7DEFF), .3f * clamp01((.04f - fabsf(d - 1)) / aa + .5f));
  if (lit(a) < .02f || disc <= 0) return p;
  // Lit: between the sunward limb and the terminator, half an ellipse of x-radius r*|cos|.
  const bool litRight = (a < .5f) != s;
  const float sx = litRight ? u : -u;
  const float w = sqrtf(fmaxf(0, 1 - v * v));
  const float cov = clamp01((sx - cosf(2 * (float)M_PI * a) * w) / aa + .5f) * disc;
  if (cov <= 0) return p;
  // Maria, upside down from the south.
  const float mu = s ? -u : u, mv = s ? -v : v;
  float keep = 1;
  for (const auto &m : MARIA) {
    float qx = (mu - m[0]) / m[2], qy = (mv - m[1]) / m[3];
    float q2 = qx * qx + qy * qy;
    if (q2 < 1) keep *= 1 - mareA(sqrtf(q2));
  }
  over(p, mix(rgb(0xF1EDD8), rgb(0x8F8A78), 1 - keep), cov);
  return p;
}

const lv_image_dsc_t *sprite(float radius, bool glow) {
  constexpr int S = 72;
  static uint8_t *buf = nullptr;
  static lv_image_dsc_t dsc;
  static int key = -1;
  const float a = age(time(nullptr));
  const bool s = south();
  // Redrawn about every 3.5 hours of phase (200 steps a month): invisible steps at this size.
  int k = (int)(a * 200) * 8 + (s ? 4 : 0) + (glow ? 2 : 0) + ((int)lroundf(radius * 4) << 12);
  if (!buf) {
    buf = (uint8_t *)heap_caps_malloc(S * S * 4, MALLOC_CAP_SPIRAM);
    if (!buf) return nullptr;
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc.header.w = S;
    dsc.header.h = S;
    dsc.header.stride = S * 4;
    dsc.data_size = S * S * 4;
    dsc.data = buf;
  }
  if (k == key) return &dsc;
  key = k;

  const float aa = 1 / radius, c = S / 2.0f;
  // The halo (display.html: drop-shadow 0 0 2 rgba(220,220,255,.55) on the lit part, a Gaussian
  // of sigma 4.8 px): the lit part's coverage, blurred by three box passes.
  // Scratch for the blur, only while drawing (internal RAM is too tight for 41 KB).
  float *halo = nullptr, *tmp = nullptr;
  if (glow) {
    halo = (float *)heap_caps_malloc(S * S * sizeof(float), MALLOC_CAP_SPIRAM);
    tmp = (float *)heap_caps_malloc(S * S * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!halo || !tmp) { heap_caps_free(halo); heap_caps_free(tmp); halo = tmp = nullptr; glow = false; }
  }
  if (glow) {
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        float u = (x + .5f - c) / radius, v = (y + .5f - c) / radius;
        float d = sqrtf(u * u + v * v);
        float cov = 0;
        if (d < 1 + aa && lit(a) >= .02f) {
          bool litRight = (a < .5f) != s;
          float sx = litRight ? u : -u, w = sqrtf(fmaxf(0, 1 - v * v));
          cov = clamp01((sx - cosf(2 * (float)M_PI * a) * w) / aa + .5f) * clamp01((1 - d) / aa + .5f);
        }
        halo[y * S + x] = cov;
      }
    constexpr int R = 4;  // 3 passes of width 9: sigma ~4.5 px
    for (int pass = 0; pass < 3; pass++) {
      for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
          float sum = 0;
          for (int i = -R; i <= R; i++) { int xx = x + i; if (xx >= 0 && xx < S) sum += halo[y * S + xx]; }
          tmp[y * S + x] = sum / (2 * R + 1);
        }
      for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
          float sum = 0;
          for (int i = -R; i <= R; i++) { int yy = y + i; if (yy >= 0 && yy < S) sum += tmp[yy * S + x]; }
          halo[y * S + x] = sum / (2 * R + 1);
        }
    }
  }
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      Px p{{0, 0, 0}, 0};
      if (glow) over(p, rgb(0xDCDCFF), .55f * halo[y * S + x]);
      Px m = pixel((x + .5f - c) / radius, (y + .5f - c) / radius, aa, a, s);
      over(p, m.c, m.a);
      uint8_t *o = buf + (y * S + x) * 4;  // B, G, R, A
      o[0] = (uint8_t)lroundf(p.c.b);
      o[1] = (uint8_t)lroundf(p.c.g);
      o[2] = (uint8_t)lroundf(p.c.r);
      o[3] = (uint8_t)lroundf(p.a * 255);
    }
  heap_caps_free(halo);
  heap_caps_free(tmp);
  return &dsc;
}

}  // namespace moon
