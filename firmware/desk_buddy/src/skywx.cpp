// Living sky's weather painter (skywx.h). A port of the painter in
// mockups/weather-sky-mockups.html, which is where the looks were chosen (2026-11-26): same
// state table, same hash and noise, so a sky there matches the device. display.html keeps a copy
// (paintWeatherSky) for the faux display.
#include "skywx.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <utility>

#include "moon.h"

// Divisions by constants become multiplications, and no errno handling: the Xtensa FPU has no
// divide instruction, and the plain build made this painter ~10x slower than the others.
#pragma GCC optimize("O2,fast-math")

namespace skywx {
namespace {

// ---------- the states ----------
enum Precip : uint8_t { NONE, DRIZZLE, RAIN, HAIL, SNOW, GRAINS };

// cover: 0..1 cloud cover; tone: 0 white fair-weather clouds .. 1 storm gray; gray/dark: how much
// the sky itself goes gray and darker (dark < 0 lightens it, for snow); sun: how much of the sun or
// moon shows (1 = clear, Home's sprite stays); amt: how heavy the rain or snow; fog 0..1; ice:
// icy tint; bolt: lightning; showers: rain heavier under the clouds.
struct Look {
  float cover, tone, gray, dark, sun;
  Precip p;
  float amt, fog, ice;
  bool bolt, showers;
};

struct Entry {
  uint8_t code;
  Look look;
};

// Open-Meteo weather codes (WMO 4677 subset).
const Entry LOOKS[] = {
    {0, {0, 0, 0, 0, 1, NONE, 0, 0, 0, false, false}},                  // clear
    {1, {.14f, 0, 0, 0, 1, NONE, 0, 0, 0, false, false}},               // mainly clear
    {2, {.48f, .12f, .1f, 0, 1, NONE, 0, 0, 0, false, false}},          // partly cloudy
    {3, {1, .62f, .75f, .12f, .12f, NONE, 0, 0, 0, false, false}},      // overcast
    {45, {1, .15f, .7f, 0, .35f, NONE, 0, .8f, 0, false, false}},       // fog
    {48, {1, .15f, .75f, 0, .3f, NONE, 0, .85f, .5f, false, false}},    // freezing (rime) fog
    {51, {.85f, .35f, .55f, .05f, .3f, DRIZZLE, .35f, 0, 0, false, false}},
    {53, {.95f, .45f, .7f, .1f, .15f, DRIZZLE, .65f, 0, 0, false, false}},
    {55, {1, .55f, .8f, .15f, .05f, DRIZZLE, 1, 0, 0, false, false}},
    {56, {1, .45f, .8f, .08f, .08f, DRIZZLE, .7f, 0, 1, false, false}},  // freezing drizzle
    {57, {1, .45f, .8f, .08f, .08f, DRIZZLE, .7f, 0, 1, false, false}},
    {61, {.9f, .5f, .65f, .1f, .15f, RAIN, .3f, 0, 0, false, false}},
    {63, {1, .65f, .8f, .2f, .05f, RAIN, .6f, 0, 0, false, false}},
    {65, {1, .8f, .88f, .32f, 0, RAIN, 1, 0, 0, false, false}},
    {66, {1, .6f, .85f, .15f, 0, RAIN, .6f, 0, 1, false, false}},  // freezing rain
    {67, {1, .6f, .85f, .15f, 0, RAIN, .6f, 0, 1, false, false}},
    {71, {.9f, .25f, .7f, -.04f, .2f, SNOW, .3f, 0, 0, false, false}},
    {73, {1, .3f, .8f, 0, .08f, SNOW, .6f, 0, 0, false, false}},
    {75, {1, .4f, .88f, .06f, 0, SNOW, 1, .25f, 0, false, false}},
    {77, {1, .3f, .8f, 0, .08f, GRAINS, .7f, 0, 0, false, false}},  // snow grains
    // Showers: the clouds of the matching rain or snow, falling a little heavier.
    {80, {.9f, .5f, .65f, .1f, .15f, RAIN, .4f, 0, 0, false, true}},
    {81, {1, .65f, .8f, .2f, .05f, RAIN, .7f, 0, 0, false, true}},
    {82, {1, .8f, .88f, .32f, 0, RAIN, 1, 0, 0, false, true}},
    {85, {.9f, .25f, .7f, -.04f, .2f, SNOW, .45f, 0, 0, false, true}},
    {86, {1, .3f, .8f, 0, .08f, SNOW, .9f, 0, 0, false, true}},
    {95, {1, .95f, .9f, .45f, 0, RAIN, .5f, 0, 0, true, false}},  // thunderstorm
    {96, {1, 1, .92f, .5f, 0, HAIL, .55f, 0, 0, true, false}},    // with hail
    {99, {1, 1, .92f, .5f, 0, HAIL, .55f, 0, 0, true, false}},
};

const Look &lookFor(int code) {
  for (const Entry &e : LOOKS)
    if (e.code == code) return e.look;
  return LOOKS[1].look;  // unknown codes: Mainly clear, like no data
}

// ---------- small color math (0..1 floats) ----------
struct V {
  float r, g, b;
};
inline V mx(const V &a, const V &b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
inline V hexV(uint32_t h) { return {((h >> 16) & 255) / 255.0f, ((h >> 8) & 255) / 255.0f, (h & 255) / 255.0f}; }
inline V fromRGB(const dither::RGB &c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f}; }
// floorf and fmodf are library calls; these are a few instructions (values stay far inside int range).
inline int ifloor(float v) {
  int i = (int)v;
  return i - (v < (float)i);
}
inline float fmodPos(float v, float p) { return v - ifloor(v / p) * p; }
inline float hyp(float a, float b) { return sqrtf(a * a + b * b); }  // hypotf goes through double
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline float sstep(float a, float b, float v) {
  float t = clamp01((v - a) / (b - a));
  return t * t * (3 - 2 * t);
}
const V WHITE{1, 1, 1}, BLACK{0, 0, 0};

// 0..1 color to RGB565 with the 8x8 Bayer dither (dither::to565, without its divisions).
inline uint16_t to565(const V &c, int x, int y) {
  const float t = (dither::BAYER8[y & 7][x & 7] + 0.5f) * (1.0f / 64);
  auto q = [t](float v, int levels) {
    int s = (int)(clamp01(v) * (levels - 1) + t);
    return s > levels - 1 ? levels - 1 : s;
  };
  return (uint16_t)((q(c.r, 32) << 11) | (q(c.g, 64) << 5) | q(c.b, 32));
}

// The mock-up's hash (Math.imul based), bit for bit: 0..1.
// gx * 73856093 wraps in 32 bits, like JavaScript's ^ on the product (signed overflow is
// undefined in C++, so multiply unsigned).
inline int32_t wrapMul(int a, uint32_t b) { return (int32_t)((uint32_t)a * b); }
inline float hsh(int32_t n) {
  uint32_t x = (uint32_t)n;
  x = (x ^ (x >> 16)) * 0x45d9f3bu;
  x = (x ^ (x >> 16)) * 0x45d9f3bu;
  x ^= x >> 16;
  return x * (1.0f / 4294967296.0f);
}

// ---------- noise: the mock-up's 64 x 64 table, in PSRAM ----------
constexpr int NS = 64;
uint8_t *noise = nullptr;
bool noiseTried = false;
bool ensureNoise() {
  if (noise || noiseTried) return noise != nullptr;
  noiseTried = true;
  noise = (uint8_t *)heap_caps_malloc(NS * NS, MALLOC_CAP_SPIRAM);
  if (!noise) {
    Serial.println("[skywx] no PSRAM for the noise table: clouds left out");
    return false;
  }
  uint32_t s = 12345;
  for (int i = 0; i < NS * NS; i++) {
    s = s * 1103515245u + 12345u;
    noise[i] = s >> 24;
  }
  return true;
}
inline float vnoise(float x, float y) {
  int xi = ifloor(x), yi = ifloor(y);
  float fx = x - xi, fy = y - yi;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  int x0 = xi & 63, y0 = yi & 63, x1 = (x0 + 1) & 63, y1 = (y0 + 1) & 63;
  float a = noise[y0 * NS + x0], b = noise[y0 * NS + x1], c = noise[y1 * NS + x0], d = noise[y1 * NS + x1];
  return ((a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy) * (1.0f / 255);
}
inline float cloudN(float X, float Y, float drift) {
  return .5f * vnoise((X + drift) / 120, Y / 42) + .3f * vnoise((X + drift * 1.5f) / 50, Y / 20) +
         .2f * vnoise((X + drift * 2) / 21, Y / 10);
}

// ---------- lightning: one fixed zigzag with a fork, right of the clock ----------
struct Pt {
  float x, y;
};
Pt boltMain[11], boltFork[5];
float boltBox[4];  // x0, y0, x1, y1
bool boltMade = false;
void makeBolt() {
  if (boltMade) return;
  boltMade = true;
  uint32_t s = 7;
  auto rnd = [&s]() {
    s = s * 1103515245u + 12345u;
    return (s >> 24) / 255.0f;
  };
  boltMain[0] = {432, 120};
  for (int i = 0; i < 10; i++) {
    float r1 = rnd(), r2 = rnd();
    boltMain[i + 1] = {boltMain[i].x + (i % 2 ? 1 : -1) * (6 + 10 * r1) - 1, boltMain[i].y + 16 + 10 * r2};
  }
  boltFork[0] = boltMain[3];
  for (int i = 0; i < 4; i++) {
    float r1 = rnd(), r2 = rnd();
    boltFork[i + 1] = {boltFork[i].x - 6 - 6 * r1, boltFork[i].y + 12 + 8 * r2};
  }
  float x0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (const Pt &p : boltMain) { x0 = fminf(x0, p.x); x1 = fmaxf(x1, p.x); y1 = fmaxf(y1, p.y); }
  for (const Pt &p : boltFork) { x0 = fminf(x0, p.x); x1 = fmaxf(x1, p.x); y1 = fmaxf(y1, p.y); }
  boltBox[0] = x0 - 22; boltBox[1] = 100; boltBox[2] = x1 + 22; boltBox[3] = y1 + 20;
}
float boltDist(float X, float Y) {
  float best = 1e9f;
  auto seg = [&](const Pt *pts, int n) {
    for (int i = 0; i < n - 1; i++) {
      float dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y;
      float t = clamp01(((X - pts[i].x) * dx + (Y - pts[i].y) * dy) / (dx * dx + dy * dy));
      float ex = X - pts[i].x - dx * t, ey = Y - pts[i].y - dy * t;
      best = fminf(best, ex * ex + ey * ey);
    }
  };
  seg(boltMain, 11);
  seg(boltFork, 5);
  return sqrtf(best);
}

// Row buffers (PSRAM, with the noise table): cloud noise sampled every 2 px on two rows, and
// the fog's bands every 4 px; the pixels between are interpolated (the clouds are smooth at that
// scale, and sampling them for every pixel was most of the paint).
float *cloudRows = nullptr;  // 2 x (481 / 2 + 2)
float *fogRow = nullptr;     // 481 / 4 + 2
constexpr int CROW = 243, FROW = 123;
bool ensureRows() {
  if (cloudRows) return true;
  cloudRows = (float *)heap_caps_malloc((2 * CROW + FROW) * sizeof(float), MALLOC_CAP_SPIRAM);
  if (!cloudRows) return false;
  fogRow = cloudRows + 2 * CROW;
  return true;
}

}  // namespace

bool coversBody(int code) { return lookFor(code).sun < 1; }

bool paint(uint16_t *buf, int n, const Frame &f, bool drawBody, const std::function<bool()> &stop) {
  const Look &S = lookFor(f.code);
  const bool haveNoise = ensureNoise() && ensureRows();
  if (S.bolt) makeBolt();
  const float k = 480.0f / n;
  const bool thumb = n != 480;
  const V top = fromRGB(f.top), bot = fromRGB(f.bot);
  // Daylight 0 (night) .. 1 (day), for lighting the clouds.
  const float day = sstep(f.rise - 0.8f, f.rise + 1.2f, f.hour) * (1 - sstep(f.set - 0.6f, f.set + 1.0f, f.hour));
  const bool night = f.night;
  const float drift = (float)f.drift;
  const float cover = haveNoise ? S.cover : 0;
  const bool full = cover >= .85f;  // mostly covered: one soft layer, not separate clouds
  const float th = .72f - cover * .42f, soft = .1f;  // cloud threshold on the noise
  const float cover01 = full ? (cover - .85f) / .15f : 0;
  const bool fogOn = S.fog > 0 && haveNoise;

  // Living sky's glow, weakened by the clouds.
  float far = 0;
  for (float cx : {0.0f, 480.0f})
    for (float cy : {0.0f, 480.0f}) far = fmaxf(far, hyp(cx - f.bx, cy - f.by));
  const float moonLit = moon::lit(f.moonAge);
  const float glowR = far * f.glowReach, glowR2 = glowR * glowR;
  const float glowAmt = (night ? f.glowNight + f.glowNightLit * moonLit : f.glowDay) * (.35f + .65f * S.sun);
  const V tint = night ? hexV(0xAAB4FF) : hexV(f.glowTint);
  const float bodyR = night ? f.moonR : f.sunR;
  const float bodyReach2 = (bodyR + 1.5f * k) * (bodyR + 1.5f * k);
  const V sunCol = hexV(0xFFC34D);

  // Cloud colors: lit by the horizon color by day, moonlit gray-blue at night.
  const V litDay = mx(WHITE, bot, .28f + .35f * (1 - day)), darkDay = mx({.36f, .40f, .47f}, {.17f, .19f, .24f}, S.tone);
  const V lit = mx({.17f, .19f, .27f}, litDay, day), dark = mx({.07f, .075f, .1f}, darkDay, day);
  const V fogCol = mx({.22f, .24f, .29f}, mx({.68f, .71f, .76f}, bot, .2f), day);
  const bool wet = S.p == DRIZZLE || S.p == RAIN || S.p == HAIL;
  const bool drz = S.p == DRIZZLE;
  const bool storm = S.bolt;
  const V slate = storm ? mx({.06f, .07f, .12f}, {.2f, .25f, .33f}, day) : mx({.16f, .2f, .3f}, {.36f, .46f, .6f}, day);
  const float blueAmt = wet ? .16f + .12f * S.amt : 0;
  const V pCol = S.p == SNOW || S.p == GRAINS ? mx({.78f, .82f, .9f}, WHITE, day)
                 : S.ice > 0              ? mx({.55f, .68f, .78f}, {.82f, .95f, 1}, day)
                                          : mx({.45f, .52f, .65f}, {.80f, .86f, .94f}, day);
  const V streakCol = wet ? mx(pCol, WHITE, .35f) : pCol;  // bold streaks are a little whiter
  const V splashCol = mx(pCol, WHITE, .3f);
  const V overhead = mx(lit, dark, S.tone);
  const V iceCol{.62f, .78f, .88f}, iceTintCol{.7f, .86f, .95f};
  const V flashCol = mx({.6f, .55f, .95f}, {.75f, .78f, .95f}, day);
  const V hidCol = mx(bot, WHITE, .3f), edgeCol = mx(bot, WHITE, .1f);
  const float hid = full ? (1 - S.sun) * day : 0;

  // The sky at height Y with the weather applied: grayer and darker under the clouds.
  auto skyAt = [&](float Y) {
    V sky = mx(top, bot, clamp01(Y / 480));
    float lum = .3f * sky.r + .59f * sky.g + .11f * sky.b;
    sky = mx(sky, {lum, lum, lum * 1.04f}, S.gray);
    if (S.dark >= 0) sky = {sky.r * (1 - S.dark), sky.g * (1 - S.dark), sky.b * (1 - S.dark)};
    else sky = mx(sky, {.86f, .9f, .95f}, -S.dark * day * 4);
    if (S.ice > 0) sky = mx(sky, iceCol, .12f * S.ice * (.3f + .7f * day));
    if (blueAmt > 0) sky = mx(sky, slate, blueAmt);
    if (full) sky = mx(sky, overhead, .7f);  // what's overhead is mostly cloud
    return sky;
  };
  // Inside a drop on the glass: the sky flipped by the lens, a darker rim, light gathered at
  // the bottom and a small glint. u, v in drop radii from its center.
  auto water = [&](float Y, float u, float v, const V &base) {
    float d = hyp(u, v);
    V wc = mx(base, skyAt(Y - v * 60), .45f);
    wc = mx(wc, WHITE, .14f + .14f * day);
    wc = mx(wc, BLACK, sstep(.55f, 1, d) * (v < .2f ? .45f : .18f));
    float cau = hyp(u, v - .45f);
    if (cau < .45f) wc = mx(wc, WHITE, (1 - cau / .45f) * .35f);
    float gl = hyp(u + .32f, v + .42f);
    if (gl < .18f) wc = mx(wc, WHITE, (1 - gl / .18f) * .8f);
    return wc;
  };

  // Per-state constants for the rain, snow and glass layers.
  const float slant = drz ? .12f : .16f + .14f * S.amt, sp = drz ? 6 : 10 - 4 * S.amt;
  const float streakDens = drz ? .4f + .5f * S.amt : .35f + .55f * S.amt, period = drz ? 26 : 70;
  const float hw = drz ? 1.5f : 1.7f, streakA = drz ? .6f + .25f * S.amt : .6f + .3f * S.amt;
  struct Layer {
    float cs, r, dens, dy;
    int sd;
  };
  Layer snow[2];
  int nSnow = 0;
  if (S.p == GRAINS) snow[nSnow++] = {9, .9f, .6f * S.amt, drift * .8f, 3};
  if (S.p == SNOW) {
    snow[nSnow++] = {30 - 10 * S.amt, 2.4f, .3f + .45f * S.amt, drift * 1.4f, 1};
    snow[nSnow++] = {15 - 4 * S.amt, 1.3f, .35f + .45f * S.amt, drift * .8f, 2};
  }
  const float splashCs = 36 - 10 * S.amt, beadCs = drz ? 18 : 28 - 6 * S.amt;

  // Caches: each column or cell's random numbers, recomputed only when x crosses into the next
  // one (they were a hash per pixel).
  struct Streak { int col = INT32_MIN; bool on; float len, off, cx; } st;
  struct Cell { int gx = INT32_MIN; bool on; float fx, fy, r, rx; } hail, snowC[2], splash, bead;
  struct Runner { int col = INT32_MIN; bool on; float r, hy, len, ph, cxBase, xHy; int cxRow = -1; float cxAt; int beadBi = INT32_MIN; bool beadOn; float bx, by, br; } run[2];

  const uint16_t flat = to565(hexV(0x0B0D11), 0, 0);
  const float edge2 = 240.0f * 240.0f;
  const int ns = n / 2 + 2;  // cloud samples per row
  float *rowA = cloudRows, *rowB = cloudRows ? cloudRows + CROW : nullptr;
  int rowAy = INT32_MIN;
  auto fillCloudRow = [&](float *dst, int ys) {
    const float Y = (ys + .5f) * k;
    for (int i = 0; i < ns; i++) dst[i] = cloudN((2 * i + .5f) * k, Y, drift);
  };

  for (int y = 0; y < n; y++) {
    if ((y & 7) == 7) {
      vTaskDelay(1);  // core 0's idle task is watched (5 s)
      if (stop && stop()) return false;
    }
    const float Y = (y + .5f) * k;
    const V sky = skyAt(Y);
    uint16_t *row = buf + y * n;
    // Cloud samples for the even rows around y.
    float fyc = 0;
    if (cover > 0) {
      const int ys0 = y & ~1;
      if (ys0 != rowAy) {
        if (ys0 == rowAy + 2) {
          std::swap(rowA, rowB);
        } else {
          fillCloudRow(rowA, ys0);
        }
        fillCloudRow(rowB, ys0 + 2);
        rowAy = ys0;
      }
      fyc = (y - ys0) * .5f;
    }
    if (fogOn) {
      for (int i = 0; i * 4 <= n + 4; i++) fogRow[i] = vnoise(((4 * i + .5f) * k + drift * .5f) / 90, Y / 22);
    }
    const float horizonN = (.5f - Y / 480) * .12f;
    const float oyC = (Y - 245) / 92, oyC2 = oyC * oyC;     // the calm oval around the text
    const float oyB = (Y - 240) / 100, oyB2 = oyB * oyB;    // where clouds thin behind it
    const float edgeBand = storm && day > 0 ? sstep(300, 470, Y) * .38f * day : 0;
    const float splashFade = wet && Y > 340 ? sstep(340, 390, Y) : 0;
    // New row: cells keyed by column only are re-keyed by this row's cell row.
    hail.gx = snowC[0].gx = snowC[1].gx = splash.gx = bead.gx = INT32_MIN;
    const int hailGy = ifloor((Y + drift * 2) / 22);
    int snowGy[2] = {0, 0};
    for (int li = 0; li < nSnow; li++) snowGy[li] = ifloor((Y + snow[li].dy) / snow[li].cs);
    const int splashGy = ifloor(Y / splashCs), beadGy = ifloor(Y / beadCs);

    for (int x = 0; x < n; x++) {
      const float X = (x + .5f) * k;
      const float cdx = X - 240;
      if (thumb && cdx * cdx + (Y - 240) * (Y - 240) > edge2) {
        row[x] = flat;
        continue;
      }
      V c = sky;
      // Glow.
      const float gdx = X - f.bx, gdy = Y - f.by, gd2 = gdx * gdx + gdy * gdy;
      if (gd2 < glowR2) {
        float g = 1 - sqrtf(gd2) / glowR;
        c = mx(c, tint, g * g * glowAmt);
      }
      // Clouds: thicker toward the top, a clearer band near the horizon.
      if (cover > 0) {
        const int i = x >> 1;
        const float fx = (x & 1) * .5f;
        const float a0 = rowA[i] + (rowA[i + 1] - rowA[i]) * fx, b0 = rowB[i] + (rowB[i + 1] - rowB[i]) * fx;
        const float nz = a0 + (b0 - a0) * fyc + horizonN;
        const float cl = full ? 1 : sstep(th - soft, th + soft, nz);
        if (cl > 0) {
          const float core = sstep(th, th + .25f, nz);
          const float shade = full ? clamp01(S.tone + (nz - .5f) * .45f) : clamp01(S.tone * (.3f + .7f * core) + (1 - core) * .1f);
          float alpha;
          if (full) {
            alpha = .82f + .18f * fmaxf(cover01, sstep(th - .1f, th + .1f, nz));
          } else {
            // Clouds thin out behind the clock and date so the text stays readable.
            const float ox = cdx / 185, behind = 1 - sstep(.6f, 1.2f, ox * ox + oyB2);
            alpha = (.75f + .25f * core) * (1 - .5f * behind * (1 - S.tone));
          }
          c = mx(c, mx(lit, dark, shade), cl * alpha);
        }
      }
      // The sun or moon, dimmed by the clouds (Home's sprite covers it when the sky is clear).
      if (drawBody && gd2 < bodyReach2) {
        const float gd = sqrtf(gd2), vis = S.sun;
        if (night) {
          moon::Px m = moon::pixel(gdx / bodyR, gdy / bodyR, k / bodyR, f.moonAge, f.south);
          if (m.a > 0) {
            V mc{m.c.r / 255.0f, m.c.g / 255.0f, m.c.b / 255.0f};
            c = mx(c, mx(c, mc, .3f + .7f * vis), m.a * fminf(1, fmaxf(vis, .04f) * 2.5f));
          }
        } else if (vis > 0) {
          float cov = clamp01((bodyR - gd) / k + .5f);
          c = mx(c, mx(c, sunCol, .3f + .7f * vis), cov * fminf(1, vis * 2.5f));
        }
      }
      if (S.ice > 0) c = mx(c, iceTintCol, S.ice * .14f * (.4f + .6f * day));
      if (blueAmt > 0) c = mx(c, slate, blueAmt * .8f);
      // Under heavy cloud the hidden sun still brightens the clouds around it: low on the left
      // and peach in the morning, high and white at midday, low on the right and warm later.
      if (hid > 0) {
        const float sd2 = gdx * gdx + gdy * gdy * 1.5625f;
        if (sd2 < 330.0f * 330) {
          float g = 1 - sqrtf(sd2) / 330;
          c = mx(c, hidCol, g * g * (storm ? .3f : .38f) * hid);
        }
      }
      float flash = 0;
      if (storm) {
        // Day: lighter low in the sky where the storm's edge lets light in (amber at sunset).
        if (edgeBand > 0) c = mx(c, edgeCol, edgeBand);
        // The flash lights the clouds around the bolt; much farther, and violet, at night.
        const float fr = 150 + 130 * (1 - day), fl2 = (X - 420) * (X - 420) + (Y - 150) * (Y - 150);
        if (fl2 < fr * fr) {
          const float t = 1 - sqrtf(fl2) / fr;
          flash = t * sqrtf(t);  // ^1.5
          c = mx(c, flashCol, flash * (.45f + .25f * (1 - day)));
        }
        if (X > boltBox[0] && X < boltBox[2] && Y > boltBox[1] && Y < boltBox[3]) {
          const float d = boltDist(X, Y);
          if (d < 14) c = mx(c, {.78f, .82f, 1}, (1 - d / 14) * (1 - d / 14) * .6f);
          if (d < 2.2f) c = mx(c, WHITE, clamp01(2.2f - d));
        }
      }
      // Rain, snow and splashes, faded in an oval around the clock and date.
      // How far outside the oval around the clock and date (0 inside .. 1 outside).
      float outside = 1;
      if (S.p != NONE) {
        const float ox = cdx / 175;
        outside = sstep(.75f, 1.15f, ox * ox + oyC2);
      }
      if (S.p != NONE) {
        const float calm = 1 - .65f * (1 - outside);
        float a = 0;
        if (wet) {
          // Bold slanted streaks: each column's streak is bright at its bottom end.
          const float u = X + Y * slant;
          const int col = ifloor(u / sp);
          if (col != st.col) {
            st.col = col;
            st.on = hsh(col * 3 + 1) < streakDens;
            st.len = (drz ? 5 : 16 + 22 * S.amt) * (.6f + .4f * hsh(col * 3 + 2));
            st.off = hsh(col * 3 + 3) * period + drift * 3;
            st.cx = .2f + .6f * hsh(col * 7 + 5);
          }
          if (st.on) {
            const float ph = fmodPos(Y + st.off, period);
            const float w = fabsf(u / sp - col - st.cx) * sp;
            if (ph < st.len && w < hw) {
              const float p = ph / st.len, head = ph > st.len * .82f ? 1.35f : 1;
              a = sqrtf(p) * sqrtf(sqrtf(p)) * clamp01(hw - w) * head * streakA;
            }
          }
          if (S.p == HAIL) {
            const float cs = 22, yy = Y + drift * 2;
            const int gx = ifloor(X / cs);
            if (gx != hail.gx) {
              hail.gx = gx;
              hail.on = hsh(gx * 911 + hailGy * 977) < .35f;
              hail.fx = gx * cs + (hsh(gx * 13 + hailGy * 7 + 1) * .7f + .15f) * cs;
              hail.fy = hailGy * cs + (hsh(gx * 5 + hailGy * 17 + 2) * .7f + .15f) * cs;
            }
            if (hail.on) {
              const float d = hyp(X - hail.fx, yy - hail.fy);
              if (d < 2.6f) c = mx(c, {.93f, .96f, 1}, clamp01(2.6f - d) * .9f * calm);
            }
          }
        } else {
          // Snow: two layers of hashed flakes, near (big, sparse) and far (small, dense).
          for (int li = 0; li < nSnow; li++) {
            const Layer &L = snow[li];
            Cell &C = snowC[li];
            const int gx = ifloor(X / L.cs), gy = snowGy[li];
            if (gx != C.gx) {
              C.gx = gx;
              C.on = hsh(wrapMul(gx, 73856093) ^ wrapMul(gy, 19349663) ^ L.sd) < L.dens;
              C.fx = gx * L.cs + (hsh(gx * 31 + gy * 17 + L.sd * 5) * .7f + .15f) * L.cs;
              C.fy = gy * L.cs + (hsh(gx * 11 + gy * 29 + L.sd * 3) * .7f + .15f) * L.cs;
            }
            if (C.on) {
              const float d = hyp(X - C.fx, Y + L.dy - C.fy);
              if (d < L.r + .6f) a = fmaxf(a, clamp01(L.r + .5f - d) * (L.sd == 1 ? .95f : .7f));
            }
          }
        }
        if (storm) a *= day + (1 - day) * (.25f + .75f * flash);  // at night the rain shows mostly in the flash
        if (a > 0) c = mx(c, wet ? streakCol : pCol, clamp01(a * calm));
        // Splashes along the bottom: a flat ripple where a drop lands, with droplets thrown up.
        if (splashFade > 0) {
          const float cs = splashCs;
          const int gx = ifloor(X / cs), gy = splashGy;
          if (gx != splash.gx) {
            splash.gx = gx;
            splash.on = hsh(wrapMul(gx, 92821) ^ wrapMul(gy, 68917)) < .25f + .4f * S.amt;
            splash.fx = gx * cs + (.25f + .5f * hsh(gx * 7 + gy * 3)) * cs;
            splash.fy = gy * cs + (.4f + .4f * hsh(gx * 5 + gy * 11)) * cs;
            splash.rx = (drz ? 3.5f : 5.5f + 2.5f * S.amt) * (.7f + .5f * hsh(gx + gy * 31));
          }
          if (splash.on) {
            const float rx = splash.rx, ry = rx * .26f;
            const float dx = X - splash.fx, dy = Y - splash.fy;
            if (fabsf(dx) < rx * 1.5f && dy > -rx * 1.3f && dy < ry * 1.5f) {
              const float e = sqrtf((dx / rx) * (dx / rx) + (dy / ry) * (dy / ry));
              float sa = 0;
              if (fabsf(e - 1) < .4f) sa = (1 - fabsf(e - 1) / .4f) * (dy < 0 ? .45f : .8f);
              static const float DROPS[3][3] = {{-.55f, -1.0f, 1.1f}, {.6f, -1.15f, 1}, {.05f, -1.5f, .9f}};
              for (const auto &dr : DROPS) {
                const float d = hyp(dx - dr[0] * rx, dy - dr[1] * rx * .7f);
                if (d < dr[2]) sa = fmaxf(sa, clamp01(dr[2] - d + .3f) * .75f);
              }
              if (sa > 0) c = mx(c, splashCol, sa * splashFade);
            }
          }
        }
      }
      // Rain on the glass: drops running down with a wet trail (some leave beads along it), and
      // still beads between them.
      if (wet) {
        const float calmG = 1 - .45f * (1 - outside);
        bool hit = false;
        for (int layer = 1; layer <= 2 && !hit; layer++) {
          const float cw = 44, off = layer == 1 ? 0 : 22;
          const int col = ifloor((X + off) / cw);
          Runner &R = run[layer - 1];
          if (col != R.col) {
            R.col = col;
            R.on = hsh(col * 7919 + layer * 104729) <= (drz ? .2f : .3f + .4f * S.amt);
            R.r = drz ? 3.4f + 1.2f * hsh(col * 31 + layer) : 5.5f + 3.5f * hsh(col * 31 + layer);
            R.hy = 70 + 360 * fmodPos(hsh(col * 53 + layer * 3) * .35f + col * .618f + layer * .5f, 1.0f);
            R.len = (drz ? 40 : 70 + 170 * S.amt) * (.5f + .7f * hsh(col * 71 + layer));
            R.ph = hsh(col * 97 + layer) * 6.28f;
            R.cxBase = col * cw - off + cw / 2 + (hsh(col * 13 + layer) - .5f) * (cw - 26);
            R.xHy = R.cxBase + 2.2f * sinf(R.hy / 23 + R.ph) + 1.2f * sinf(R.hy / 9 + R.ph * 2);
            R.cxRow = -1;
            R.beadBi = INT32_MIN;
          }
          if (!R.on || !(Y > R.hy - R.len - 2 && Y < R.hy + R.r * 1.2f)) continue;
          const float r = R.r, hy = R.hy, len = R.len;
          auto wob = [&](float yy) { return R.cxBase + 2.2f * sinf(yy / 23 + R.ph) + 1.2f * sinf(yy / 9 + R.ph * 2); };
          if (fabsf(X - R.xHy) > r + 14) continue;  // nowhere near this drip, its trail or its beads
          if (R.cxRow != y) {  // the trail's x on this row (two sines): once per row, not per pixel
            R.cxRow = y;
            R.cxAt = wob(Y);
          }
          const float cx = R.cxAt;
          if (Y > hy - r * 1.4f) {
            // The drop: round at the bottom, drawn out toward the top.
            const float u0 = (X - R.xHy) / r, v = (Y - hy) / r;
            const float w = v < 0 ? 1 - .5f * fminf(1, -v / 1.4f) : 1;
            const float u = u0 / w, d = v < 0 ? hyp(u, v / 1.4f) : hyp(u, v);
            if (d < 1) {
              c = mx(c, water(Y, u, v < 0 ? v / 1.4f : v, c), clamp01((1 - d) * r * 1.4f) * calmG);
              hit = true;
              continue;
            }
          }
          // The trail: a thin wet streak that narrows toward the top.
          const float t = (hy - Y) / len, tw = r * (.5f - .3f * t);
          if (t > 0 && t < 1 && fabsf(X - cx) < tw + .8f) {
            const float e = fabsf(X - cx) / tw;
            V tc = mx(c, WHITE, (.12f + .1f * day) * (1 - e) * (1 - .6f * t));
            tc = mx(tc, BLACK, .22f * sstep(.45f, 1, e) * (1 - .6f * t));
            c = mx(c, tc, clamp01(tw + .8f - fabsf(X - cx)) * calmG);
            hit = true;
          }
          // Beads the drop left behind.
          const int bi = ifloor((hy - Y + 6) / 26);
          if (bi != R.beadBi) {  // the bead in this 26-row band, if any (once per band, not per pixel)
            R.beadBi = bi;
            R.beadOn = bi >= 1 && bi * 26 < len && hsh(col * 331 + bi * 17 + layer) < .5f;
            if (R.beadOn) {
              R.by = hy - bi * 26 + 6 - 8 * hsh(col + bi);
              R.bx = wob(R.by) + (hsh(col * 3 + bi) - .5f) * 6;
              R.br = 1.8f + 1.6f * hsh(col * 5 + bi);
            }
          }
          if (R.beadOn) {
            const float bx = R.bx, by = R.by, br = R.br;
            const float u = (X - bx) / br, v = (Y - by) / br, d = hyp(u, v);
            if (d < 1) {
              c = mx(c, water(Y, u, v, c), clamp01((1 - d) * br * 1.5f) * calmG);
              hit = true;
            }
          }
        }
        if (!hit) {
          const float cs = beadCs;
          const int gx = ifloor(X / cs), gy = beadGy;
          if (gx != bead.gx) {
            bead.gx = gx;
            bead.on = hsh(wrapMul(gx, 19349663) ^ wrapMul(gy, 83492791) ^ 5) < (drz ? .6f : .35f + .25f * S.amt);
            bead.r = (drz ? 1.3f : 1.8f) + (drz ? 1.5f : 2.2f) * hsh(gx * 3 + gy * 7);
            bead.fx = gx * cs + bead.r + 1 + (cs - 2 * bead.r - 2) * hsh(gx * 17 + gy * 5);
            bead.fy = gy * cs + bead.r + 1 + (cs - 2 * bead.r - 2) * hsh(gx * 23 + gy * 13);
          }
          if (bead.on) {
            const float u = (X - bead.fx) / bead.r, v = (Y - bead.fy) / bead.r;
            if (fabsf(u) < 1 && fabsf(v) < 1) {
              const float d = hyp(u, v);
              if (d < 1) c = mx(c, water(Y, u, v, c), clamp01((1 - d) * bead.r * 1.5f) * calmG * .9f);
            }
          }
        }
      }
      // Fog: stronger toward the bottom, in soft bands.
      if (fogOn) {
        const int i = x >> 2;
        const float band = fogRow[i] + (fogRow[i + 1] - fogRow[i]) * (x & 3) * .25f;
        c = mx(c, fogCol, clamp01(S.fog * (.3f + .7f * Y / 480) * (.75f + .45f * band)));
      }
      // Bright covered skies (snow, fog, pale overcast): darken softly behind the clock and date so
      // the white text stays readable. Only where the sky there is bright, and not on the white
      // fair-weather clouds of a partly cloudy sky.
      if (full) {
        const float ox = cdx / 175, inside = 1 - sstep(0, 2.4f, ox * ox + oyC2);  // wide and soft: no visible oval
        if (inside > 0) {
          const float lum = .3f * c.r + .59f * c.g + .11f * c.b;
          if (lum > .45f) c = mx(c, BLACK, (lum - .45f) * .7f * inside);
        }
      }
      row[x] = to565(c, x, y);
    }
  }
  return true;
}

}  // namespace skywx
