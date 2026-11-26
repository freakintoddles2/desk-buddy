// Home, with three faces (home-mockups.html; picked in home-customize-mockups.html). Each face
// is one entry in FACES below; paintScene() draws any of them from the entry's fields:
//  - Sun ring (mock-up I): a 24-hour sky ring with noon at the top and midnight at the bottom,
//    colored by the sky at each hour, bright for the hours already passed. The sun (or a
//    crescent moon at night) rides the ring at the current time, with a soft glow behind it.
//  - Living sky (mock-up A): the whole background is the sky outside, top to bottom, and the
//    sun or moon rides an arc across the upper half.
//  - Plain: the dark background only.
// Clock and date in the middle; the weather line and the glance stats can each be turned off.
//
// Backgrounds are computed pixel by pixel into an RGB565 image once a minute on a background
// task (painter.h), so the face is cheap to redraw while swiping: LVGL only copies the image.
//
// Swipe up from the bottom edge opens the customize sheet: a carousel of face previews (the
// middle one is Home as it will look, with your elements) and four element toggles.
#include <atomic>
#include <esp_heap_caps.h>
#include <functional>
#include <math.h>
#include <mutex>

#include "app.h"
#include "clock.h"
#include "daynum.h"
#include "dial.h"
#include "dither.h"
#include "heat_assets.h"
#include "records.h"
#include "painter.h"
#include "icons.h"
#include "moon.h"
#include "net.h"
#include "sprites.h"
#include "store.h"
#include "theme.h"
#include "timers.h"
#include "skywx.h"
#include "sun.h"
#include "tz.h"
#include "weather.h"

LV_FONT_DECLARE(font_clock_120);
LV_FONT_DECLARE(font_heat_120);

namespace {

// ---------- settings ----------
enum Show : uint8_t { SHOW_WEATHER = 1, SHOW_HABITS = 2, SHOW_COUNTDOWN = 4, SHOW_TIMERS = 8, SHOW_ALL = 15 };
uint8_t shown() { return (uint8_t)store::getInt("homeshow", SHOW_ALL); }
bool shows(uint8_t bit) { return shown() & bit; }


constexpr int W = 480;
constexpr float PX = 4.8f;            // display.html draws in a 100-unit viewBox on 480 px
constexpr float RING_R = 46.2f * PX;  // centerline radius
constexpr float RING_HALF = 3.4f * PX / 2;
constexpr float FUTURE_OPA = 0.28f;   // hours still to come today
constexpr uint32_t SCREEN_HEX = 0x0B0D11;

using dither::RGB;
using dither::mix;
using dither::rgb;
using dither::to565;

// ---------- faces ----------
// Every Home face is one entry in FACES. The saved choice (NVS "homeface") is the entry's index,
// so add new faces at the END and never reorder or remove one: a saved index would silently
// point at a different face. Everything else (the customize sheet, the knob, the previews) reads
// FACE_COUNT and these fields.
//
// What paintScene() draws, bottom to top: the background (dark, or the sky's gradient for the
// time of day), the 24-hour ring if on, then a glow around the sun or moon. The sun/moon itself
// is a sprite on top on Home (sprites.h, moon.h) and painted in on the previews. A face that
// needs more (Heat's palms and skyline) sets paint to its own painter instead.
// Paint budget: Sun ring ~600 ms per full 480 px paint on core 0, once a minute; keep a new face
// under ~1 s (~2 s felt laggy in testing). Living sky with weather (skywx.cpp) is ~0.3 s clear,
// ~0.6-0.8 s cloudy or foggy, ~1.2-1.6 s rain or snow and ~2 s a storm (2026-11-26, measured on
// core 0); the last painted sky stays up meanwhile, so only the once-a-minute update waits.
enum class Bg : uint8_t { DARK, SKY };     // the screen color, or the sky's top-to-horizon gradient
// The sun/moon rides the ring, or an arc over the upper half; SINK: the sun stays in the middle
// and sinks toward the horizon through the day (Heat), the moon takes the arc.
enum class Path : uint8_t { RING, ARC, SINK };

struct Scene;
// A face's own painter: fill the n x n buffer for the scene (n = 480 for Home, smaller for the
// previews, which need the moon painted in at night and the corners outside the circle in
// SCREEN_HEX). Call stop() every few rows and return false if it says so; yield (vTaskDelay(1))
// every 8 rows, since this runs on core 0 and its idle task is watched.
using PaintFn = bool (*)(uint16_t *buf, int n, const Scene &s, const std::function<bool()> &stop);

// Text colors (0xRRGGBB; 0 = the usual): the clock, the date, the weather's "82° Clear", the
// secondary text (stat labels, and the rest of the weather line unless wx is set: then the whole
// line is wx), and a drop shadow behind the clock.
struct Ink {
  uint32_t clock = 0, date = 0, wx = 0, dim = 0, shadow = 0;
};

struct FaceDef {
  const char *name;            // for logs only (the sheet shows no names)
  bool body = true;            // sun by day, moon at night. false: the flat dark screen, never repainted
  Bg bg = Bg::DARK;
  bool ring = false;           // the 24-hour sky ring
  Path path = Path::RING;      // where the sun/moon goes (RING follows the ring even when it's off)
  float sunR = 16, moonR = 15.84f;  // radius in px on the 480 px screen
  bool moonHalo = false;       // Home's moon sprite gets a soft halo (looks right on a dark face)
  float glowReach = 0.38f;     // glow radius, as a fraction of the distance to the farthest corner
  float glowDay = 0.22f;       // glow strength by day, 0..1
  float glowNight = 0.03f, glowNightLit = 0.09f;  // at night: base + this x the moon's lit fraction
  uint32_t glowTint = 0;       // day glow color (0xRRGGBB); 0 = the horizon color at this hour
  Ink day, night;              // text colors by day and at night (Home rebuilds at sunrise/sunset if they differ)
  bool lightBg = false;        // bright background: white selection bar, stat colors kept readable
  PaintFn paint = nullptr;     // own painter instead of the fields above (they still set the moon)
  bool daySprite = true;       // false: the painter draws the sun itself, Home shows no sun sprite
  const lv_font_t *clockFont = nullptr;  // nullptr: font_clock_120; keep the same 148 px line height
  bool weather = false;        // the sky follows the weather (skywx.h): clouds, rain, snow, fog, storms
  bool upper = false;          // the date in capitals, spaced out (the weather line would be too wide)
};

bool paintHeat(uint16_t *buf, int n, const Scene &s, const std::function<bool()> &stop);
bool paintWeatherSky(uint16_t *buf, int n, const Scene &s, const std::function<bool()> &stop);

const FaceDef FACES[] = {
    // 0: Sun ring (mock-up I), the default.
    {.name = "Sun ring", .ring = true, .moonHalo = true},
    // 1: Living sky (mock-up A).
    {.name = "Living sky", .bg = Bg::SKY, .path = Path::ARC, .sunR = 22, .moonR = 20.4f, .glowReach = 0.30f,
     .glowDay = 0.35f, .glowNight = 0.025f, .glowNightLit = 0.075f, .glowTint = 0xFFF1C8,
     .day = {.dim = 0xE6ECF5}, .night = {.dim = 0xE6ECF5}, .lightBg = true, .paint = paintWeatherSky, .weather = true},
    // 2: Plain.
    {.name = "Plain", .body = false},
    // 3: Heat (a coworker's concept, TEST ONLY): synthwave sunset with palms by day, a neon skyline
    // after dark, condensed slanted clock. Painted by paintHeat (art from tools/gen_heat.py).
    {.name = "Heat", .path = Path::SINK, .moonR = 20.4f, .moonHalo = true,
     .day = {.clock = 0xFFF3E0, .date = 0x3B0D3F, .wx = 0x3B0D3F, .dim = 0xFFC9E3, .shadow = 0x3A0F45},
     .night = {.clock = 0xFF4F9A, .date = 0xD9C6FF, .wx = 0x5EF0DC, .dim = 0xC9B8F0, .shadow = 0x5A0F3A},
     .paint = paintHeat, .daySprite = false, .clockFont = &font_heat_120, .upper = true},
    // New faces go here.
};
constexpr int FACE_COUNT = sizeof(FACES) / sizeof(FACES[0]);

int face() { return constrain(store::getInt("homeface", 0), 0, FACE_COUNT - 1); }

// Sky colors through the day, keyed to sunrise/sunset: the horizon (bottom, also the ring's
// colors) and the top of the sky (Living sky's gradient; home-mockups.html SKY).
struct SkyKeys {
  float h[10];
  RGB c[10], top[10];
  explicit SkyKeys(const SunTimes &st) {
    const float rise = st.rise, set = st.set;
    const float hs[10] = {0, rise - 1.6f, rise, rise + 1.3f, rise + 3.5f, set - 3, set - 1.2f, set, set + 1.1f, 24};
    const uint32_t cs[10] = {0x1b1d4a, 0x2a2458, 0xe98a6a, 0xf2b77c, 0x5ea9ea,
                             0x5aa2e4, 0xf19a5c, 0xe1566e, 0x4a2a6e, 0x1b1d4a};
    const uint32_t ts[10] = {0x0a0f2c, 0x0f1538, 0x3b4a8f, 0x3f7fd6, 0x2a6fd0,
                             0x2766c4, 0x4a5bb0, 0x3b2a74, 0x191a4c, 0x0a0f2c};
    for (int i = 0; i < 10; i++) { h[i] = hs[i]; c[i] = rgb(cs[i]); top[i] = rgb(ts[i]); }
  }
  int seg(float hour, float &t) const {
    int i = 0;
    while (i < 8 && h[i + 1] <= hour) i++;
    t = fmaxf(0, fminf(1, (hour - h[i]) / (h[i + 1] - h[i])));
    return i;
  }
  RGB at(float hour) const { float t; int i = seg(hour, t); return mix(c[i], c[i + 1], t); }
  RGB topAt(float hour) const { float t; int i = seg(hour, t); return mix(top[i], top[i + 1], t); }
};

// Degrees clockwise from the top: noon at the top, midnight at the bottom.
float ringDeg(float h) { return (h - 12) / 24 * 360; }

// Brightness of the ring at hour h: hours already passed today are bright, the rest faded.
// Midnight (the bottom) would then be a hard edge between "just before" and "just after", so
// the brightness crossfades over MIDNIGHT_FADE hours on each side and both sides meet halfway.
constexpr float MIDNIGHT_FADE = 1.5f;
float ringOpa(float h, float hour) {
  auto base = [hour](float x) { return x <= hour ? 1.0f : FUTURE_OPA; };
  float meet = (base(0) + base(23.999f)) / 2;
  if (h < MIDNIGHT_FADE) return meet + (base(h) - meet) * (h / MIDNIGHT_FADE);
  if (h > 24 - MIDNIGHT_FADE) return base(h) + (meet - base(h)) * ((h - (24 - MIDNIGHT_FADE)) / MIDNIGHT_FADE);
  return base(h);
}

// Everything a face needs to paint itself for one minute.
struct Scene {
  int face;
  SkyKeys sky;
  float hour;
  bool night;
  float bx, by;  // the sun or moon, in 480 px coordinates
  float moonAge = 0;   // 0..1 through the month (moon.h)
  bool south = false;  // south of the equator: the moon is seen upside down
  int wx = skywx::NO_DATA;  // the weather a weather face shows (WMO code, skyCode())
  int drift = 0;            // minutes: the clouds drift and the rain moves from one paint to the next
};

// The sun's (or moon's) position for a face at this time.
void bodyPos(int f, const SunTimes &st, float hour, bool night, float &x, float &y) {
  if (FACES[f].path == Path::SINK && !night) {
    float p = fmaxf(0, fminf(1, (hour - st.rise) / (st.set - st.rise)));
    x = W / 2.0f;
    y = 268 + 80 * p;  // Heat: from high in the morning to mostly under the horizon (y 318) at sunset
    return;
  }
  if (FACES[f].path == Path::ARC || FACES[f].path == Path::SINK) {
    // An arc across the upper half: rises at the left, highest at midday (or midnight for the
    // moon), sets at the right.
    float p;
    if (!night) p = (hour - st.rise) / (st.set - st.rise);
    else {
      float len = 24 - (st.set - st.rise);
      p = (hour >= st.set ? hour - st.set : hour + 24 - st.set) / len;
    }
    p = fmaxf(0, fminf(1, p));
    x = W / 2.0f - 168 * cosf(M_PI * p);
    y = 236 - 150 * sinf(M_PI * p);
    return;
  }
  float a = ringDeg(hour) * M_PI / 180;
  x = W / 2.0f + RING_R * sinf(a);
  y = W / 2.0f - RING_R * cosf(a);
}

// Paint a face's background into an n x n RGB565 buffer (n = 480 for Home, smaller for the
// customize previews, which also get the sun or moon painted in and the corners outside the
// circle filled with the screen color). Runs on core 0; yields so the idle watchdog is fed.
//
// Fast path (a new face was slow to appear with the float version, ~1.8 s): colors in
// 8.8 fixed point, the glow from a lookup table on squared distance, integer ordered dithering;
// square roots and angles only for the ring band and the sun disc.
static inline uint16_t dither565(int r8, int g8, int b8, int x, int y) {
  // c8 = channel << 8 (0..65280). Level = floor(c/255*(L-1) + (bayer+0.5)/64), in integers.
  int t = dither::BAYER8[y & 7][x & 7] * 1020 + 510;
  int r = (r8 * 31 + t) / 65280, g = (g8 * 63 + t) / 65280, b = (b8 * 31 + t) / 65280;
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  if (r < 0) r = 0;
  if (g < 0) g = 0;
  if (b < 0) b = 0;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// The background under everything else at height Y (480 px units), before the ring and glow.
inline RGB baseAt(const FaceDef &F, const RGB &screen, const RGB &top, const RGB &bottom, float Y) {
  return F.bg == Bg::SKY ? mix(top, bottom, fminf(1, Y / W)) : screen;
}

// The glow table below is shared, and Home's painter and the customize previews both paint on
// core 0, yielding every 8 rows: one at a time, or one would paint with the other's table.
std::mutex paintMutex;

// stop (optional) is checked every 8 rows: true abandons the paint (returns false, the buffer
// half painted). The customize previews use it when the face they're painting scrolls away.
bool paintScene(uint16_t *buf, int n, const Scene &s, const std::function<bool()> &stop = {}) {
  const FaceDef &F = FACES[s.face];
  if (F.paint) {
    std::lock_guard<std::mutex> lock(paintMutex);
    return F.paint(buf, n, s, stop);
  }
  const RGB screen = rgb(SCREEN_HEX);
  const float k = (float)W / n;  // 480 px units per buffer pixel
  const bool thumb = n != W;
  const uint16_t flat = to565(screen, 0, 0);
  if (!F.body) {
    for (int i = 0; i < n * n; i++) buf[i] = flat;
    return true;
  }
  std::lock_guard<std::mutex> lock(paintMutex);
  // Glow around the sun or moon.
  RGB tint = s.night ? rgb(0xAAB4FF) : (F.glowTint ? rgb(F.glowTint) : s.sky.at(s.hour));
  float far = 0;
  for (int cx : {0, W})
    for (int cy : {0, W}) far = fmaxf(far, hypotf(cx - s.bx, cy - s.by));
  const float glowR = far * F.glowReach;
  // At night the glow follows how much of the moon is lit (a thin crescent barely glows).
  const float moonLit = moon::lit(s.moonAge);
  const float glowAmt = s.night ? F.glowNight + F.glowNightLit * moonLit : F.glowDay;
  // Glow strength (0..256) by squared distance.
  constexpr int LUT = 512;
  static uint16_t glowLut[LUT + 1];
  const float glowR2 = glowR * glowR, lutScale = LUT / glowR2;
  for (int i = 0; i <= LUT; i++) {
    float g = 1 - sqrtf(i / lutScale) / glowR;
    glowLut[i] = (uint16_t)lroundf(fmaxf(0, g) * fmaxf(0, g) * glowAmt * 256);
  }
  const int tr = lroundf(tint.r * 256), tg = lroundf(tint.g * 256), tb = lroundf(tint.b * 256);
  const RGB bodyCol = s.night ? rgb(0xDFE6FF) : rgb(0xFFC34D);
  const float bodyR = s.night ? F.moonR : F.sunR;
  const float bodyR2 = (bodyR * 1.04f + k) * (bodyR * 1.04f + k);
  const float rIn = RING_R - RING_HALF, rOut = RING_R + RING_HALF;
  const float band0 = (rIn - 1) * (rIn - 1), band1 = (rOut + 1) * (rOut + 1);
  const float edge2 = (W / 2.0f) * (W / 2.0f);
  const RGB top = s.sky.topAt(s.hour), bottom = s.sky.at(s.hour);

  for (int y = 0; y < n; y++) {
    if ((y & 7) == 7) {
      vTaskDelay(1);  // core 0's idle task is watched (5 s)
      if (stop && stop()) return false;
    }
    const float Y = (y + 0.5f) * k;
    const RGB rb = baseAt(F, screen, top, bottom, Y);
    const int r0 = lroundf(rb.r * 256), g0 = lroundf(rb.g * 256), b0 = lroundf(rb.b * 256);
    const float dy = Y - W / 2.0f, dy2 = dy * dy;
    const float gy = Y - s.by, gy2 = gy * gy;
    uint16_t *row = buf + y * n;
    for (int x = 0; x < n; x++) {
      const float X = (x + 0.5f) * k;
      const float dx = X - W / 2.0f, d2 = dx * dx + dy2;
      if (thumb && d2 > edge2) { row[x] = flat; continue; }
      const float gx = X - s.bx, gd2 = gx * gx + gy2;
      int r = r0, g = g0, b = b0;
      if (gd2 < glowR2) {
        int a = glowLut[(int)(gd2 * lutScale)];
        r += ((tr - r0) * a) >> 8;
        g += ((tg - g0) * a) >> 8;
        b += ((tb - b0) * a) >> 8;
      }
      bool ring = F.ring && d2 > band0 && d2 < band1;
      bool body = thumb && gd2 < bodyR2;
      if (ring || body) {  // the rare pixels: float math
        RGB c{r / 256.0f, g / 256.0f, b / 256.0f};
        if (ring) {
          float d = sqrtf(d2);
          float cover = fminf(1, fmaxf(0, rOut - d + 0.5f * k)) * fminf(1, fmaxf(0, d - rIn + 0.5f * k));
          float deg = atan2f(dx, -dy) * 180 / M_PI;  // clockwise from the top
          float h = fmodf(12 + deg / 15 + 24, 24);
          c = mix(c, s.sky.at(h), cover * ringOpa(h, s.hour));
        }
        if (body && s.night) {  // the previews have no sprite on top, so the body is painted in
          moon::Px m = moon::pixel(gx / bodyR, gy / bodyR, k / bodyR, s.moonAge, s.south);
          if (m.a > 0) c = mix(c, m.c, m.a);
        } else if (body) {
          float cov = fminf(1, fmaxf(0, (bodyR - sqrtf(gd2)) / k + 0.5f));
          if (cov > 0) c = mix(c, bodyCol, cov);
        }
        r = lroundf(c.r * 256); g = lroundf(c.g * 256); b = lroundf(c.b * 256);
      }
      row[x] = dither565(r, g, b, x, y);
    }
  }
  return true;
}

// ---------- Heat face painter (TEST: a coworker's concept) ----------
// Day: a synthwave sunset. Sky gradient purple to amber down to the horizon (y 318, just under
// Home's weather line, so the line doesn't cross the text), a big sun
// with cut-out stripes in its lower half that sinks through the day (bodyPos SINK), its glow,
// a bright horizon line, dark water with the sun's reflection as bars, palm silhouettes on top.
// Night: purple sky with a few stars, the moon's glow (the moon is the Home sprite), and a
// skyline with lit windows. Art (palms, buildings, windows) from tools/gen_heat.py.
namespace heat {
constexpr float HY = 318;    // day horizon
constexpr float SUN_R = 92;
const uint32_t SKY_DAY[][2] = {{0, 0x2A0E5E}, {128, 0x6C238F}, {222, 0xC2348A}, {282, 0xFF6A4F}, {318, 0xFFAD4A}};
const uint32_t SKY_NIGHT[][2] = {{0, 0x0B0420}, {264, 0x24093F}, {400, 0x4A1660}, {480, 0x6A1F6A}};

template <size_t N>
RGB gradient(const uint32_t (&stops)[N][2], float y) {
  if (y <= stops[0][0]) return rgb(stops[0][1]);
  for (size_t i = 1; i < N; i++)
    if (y <= stops[i][0])
      return mix(rgb(stops[i - 1][1]), rgb(stops[i][1]), (y - stops[i - 1][0]) / (float)(stops[i][0] - stops[i - 1][0]));
  return rgb(stops[N - 1][1]);
}

// Alpha (0..1) of the palm masks at screen pixel (x, y).
float palmAt(int x, int y) {
  float a = 0;
  for (const HeatMask &m : HEAT_PALMS) {
    int i = x - m.x, j = y - m.y;
    if (i < 0 || j < 0 || i >= m.w || j >= m.h) continue;
    int k = j * m.w + i;
    a = fmaxf(a, ((m.px[k >> 1] >> ((k & 1) ? 0 : 4)) & 15) / 15.0f);
  }
  return a;
}
}  // namespace heat

bool paintHeat(uint16_t *buf, int n, const Scene &s, const std::function<bool()> &stop) {
  using namespace heat;
  const float k = (float)W / n;
  const bool thumb = n != W;
  const uint16_t flat = to565(rgb(SCREEN_HEX), 0, 0);
  const float edge2 = (W / 2.0f) * (W / 2.0f);
  // Skyline: the top of the buildings in each column (480 px), from the generated table.
  static int16_t roof[W];
  static bool roofReady = false;
  if (!roofReady) {
    for (int x = 0; x < W; x++) roof[x] = W;
    for (const HeatBuilding &b : HEAT_BUILDINGS)
      for (int x = b.x0; x < b.x1 && x < W; x++) roof[x] = min(roof[x], b.top);
    roofReady = true;
  }
  const float moonLitAmt = moon::lit(s.moonAge);
  const float far = 340;  // roughly the farthest corner from the moon
  const float glowR = far * 0.32f, glowAmt = 0.05f + 0.10f * moonLitAmt;
  const float moonR = FACES[s.face].moonR;
  const RGB sunTop = rgb(0xFFE27A), sunBottom = rgb(0xFF5F7E), halo = rgb(0xFF9A5A);
  const RGB water0 = rgb(0x3A1450), water1 = rgb(0x0E0820), line = rgb(0xFFD27A);
  const RGB palm = rgb(0x12061C), building = rgb(0x07030D), rim = rgb(0x5A1D5F), moonGlow = rgb(0xC8B4FF);

  for (int y = 0; y < n; y++) {
    if ((y & 7) == 7) {
      vTaskDelay(1);  // core 0's idle task is watched (5 s)
      if (stop && stop()) return false;
    }
    const float Y = (y + 0.5f) * k;
    const RGB skyRow = s.night ? gradient(SKY_NIGHT, Y) : gradient(SKY_DAY, Y);
    // Per row (day): the sun's color and whether this row is a stripe gap; under the horizon,
    // the water and which reflection bar (if any) this row is on.
    const float gy = Y - s.by, gy2 = gy * gy;
    RGB sunRow = mix(sunTop, sunBottom, fminf(1, fmaxf(0, (Y - (s.by - SUN_R)) / (2 * SUN_R))));
    bool gap = false;
    const float y0 = s.by - 0.05f * SUN_R;
    if (Y > y0) gap = fmodf(Y - y0, 16) < 1.5f + 6.5f * (Y - y0) / (SUN_R * 1.05f);
    RGB waterRow = mix(water0, water1, (Y - HY) / (W - HY)), barCol{0, 0, 0};
    float barHalf = -1, barAmt = 0;
    if (!s.night && Y >= HY) {
      float yy = HY + 7;
      for (int i = 0; i < 8; i++) {
        float h = 2.5f + 0.7f * i;
        if (Y >= yy && Y < yy + h) {
          barHalf = SUN_R * (0.95f - 0.08f * i);
          barCol = mix(rgb(0xFFB347), rgb(0xFF4F8B), i / 7.0f);
          barAmt = 0.85f - 0.08f * i;
          break;
        }
        yy += 9 + 3 * i;
      }
    }
    const float haloR = SUN_R * 2.4f, haloR2 = haloR * haloR, glowR2 = glowR * glowR;
    const bool nearLine = fabsf(Y - HY) < 1.5f;
    uint16_t *row = buf + y * n;
    for (int x = 0; x < n; x++) {
      const float X = (x + 0.5f) * k;
      const float dx = X - W / 2.0f, dy = Y - W / 2.0f;
      if (thumb && dx * dx + dy * dy > edge2) { row[x] = flat; continue; }
      RGB c = skyRow;
      const float gx = X - s.bx, gd2 = gx * gx + gy2;
      if (!s.night) {
        if (Y < HY) {
          // glow, then the sun: a top-to-bottom gradient with stripes cut out of its lower half,
          // each gap a bit thicker than the one above
          if (gd2 < haloR2) {
            const float gd = sqrtf(gd2), g = 1 - gd / haloR;
            c = mix(c, halo, 0.35f * g * g);
            if (!gap) {
              float cov = fminf(1, fmaxf(0, (SUN_R - gd) / k + 0.5f));
              if (cov > 0) c = mix(c, sunRow, cov);
            }
          }
        } else {
          c = waterRow;  // with the sun's reflection: bars, thinner and narrower going down
          if (barHalf > 0 && fabsf(gx) < barHalf) c = mix(c, barCol, barAmt);
        }
        if (nearLine) c = mix(c, line, 0.8f);
        float a = heat::palmAt((int)X, (int)Y);
        if (a > 0) c = mix(c, palm, a);
      } else {
        const float gd = sqrtf(gd2);
        // a few stars (a fixed hash of the 480 px position), the moon's glow, the skyline
        if (Y < 330) {
          uint32_t h = ((uint32_t)X * 73856093u) ^ ((uint32_t)Y * 19349663u);
          h = (h ^ (h >> 13)) * 0x5bd1e995u;
          if ((h & 0xFFFF) < 10) c = mix(c, rgb(0xFFFFFF), 0.35f + ((h >> 16) & 0xFF) / 512.0f);
        }
        if (gd2 < glowR2) { float g = 1 - gd / glowR; c = mix(c, moonGlow, glowAmt * g * g); }
        int xi = min(W - 1, (int)X);
        if (Y >= roof[xi]) {
          c = Y - roof[xi] < 2 ? rim : building;
          for (const HeatWindow &w : HEAT_WINDOWS)
            if (X >= w.x && X < w.x + 4 && Y >= w.y && Y < w.y + 4) { c = rgb(HEAT_WINDOW_COLORS[w.color]); break; }
        }
        if (thumb && gd < moonR * 1.04f + k) {  // previews have no sprite: paint the moon in
          moon::Px m = moon::pixel(gx / moonR, gy / moonR, k / moonR, s.moonAge, s.south);
          if (m.a > 0) c = mix(c, m.c, m.a);
        }
      }
      row[x] = dither565(lroundf(c.r * 256), lroundf(c.g * 256), lroundf(c.b * 256), x, y);
    }
  }
  return true;
}

// The weather Living sky shows: the current conditions; when those are 3 hours old or more (Wi-Fi
// down), the forecast for this hour; with no weather data at all, Mainly clear (2026-11-26).
// Cached per weather update and hour (the UI thread asks every minute and for every preview).
int skyTestCode = -1;  // serial X<code>: show this code instead (testing)
uint32_t skyTestGen = 0;  // bumped when it changes, so Home repaints
int skyCode() {
  static uint32_t ver = ~0u;
  static long hourKey = -1;
  static int code = skywx::NO_DATA;
  if (skyTestCode >= 0) return skyTestCode;
  time_t t = time(nullptr);
  if (weather::version() == ver && t / 3600 == hourKey) return code;
  ver = weather::version();
  hourKey = t / 3600;
  code = skywx::NO_DATA;
  weather::Data d;
  if (weather::get(d)) {
    if (t - d.fetched < 3 * 3600) {
      code = d.now.code;
    } else {
      for (const weather::Hour &h : d.hours)
        if (h.at <= t && t < h.at + 3600) { code = h.code; break; }
    }
  }
  return code;
}

// Scene for the current minute.
Scene sceneNow(int f, const struct tm &now) {
  float hour = now.tm_hour + now.tm_min / 60.0f;
  float lat, lon;
  if (!weather::coords(lat, lon)) tz::fallbackCoords(lat, lon);  // before the first forecast, or no Wi-Fi
  SunTimes st = sunTimes(now, clockx::utcOffsetHours(), lat, lon);
  bool night = hour < st.rise || hour > st.set;
  Scene s{f, SkyKeys(st), hour, night, 0, 0};
  s.moonAge = moon::age(time(nullptr));
  s.south = lat < 0;
  bodyPos(f, st, hour, night, s.bx, s.by);
  if (FACES[f].weather) {
    s.wx = skyCode();
    s.drift = now.tm_hour * 60 + now.tm_min;
  }
  return s;
}

// Hides the sun or moon sprite: clouds cover it, and the painter draws it dimmed.
bool cloudsCoverBody(const Scene &s) { return FACES[s.face].weather && skywx::coversBody(s.wx); }

// Living sky with the weather (skywx.cpp). The previews have no sprite, so the body is painted
// in there; on Home only when clouds cover it.
bool paintWeatherSky(uint16_t *buf, int n, const Scene &s, const std::function<bool()> &stop) {
  const FaceDef &F = FACES[s.face];
  skywx::Frame f;
  f.top = s.sky.topAt(s.hour);
  f.bot = s.sky.at(s.hour);
  f.hour = s.hour;
  f.rise = s.sky.h[2];
  f.set = s.sky.h[7];
  f.night = s.night;
  f.bx = s.bx;
  f.by = s.by;
  f.sunR = F.sunR;
  f.moonR = F.moonR;
  f.glowReach = F.glowReach;
  f.glowDay = F.glowDay;
  f.glowNight = F.glowNight;
  f.glowNightLit = F.glowNightLit;
  f.glowTint = F.glowTint;
  f.moonAge = s.moonAge;
  f.south = s.south;
  f.code = s.wx;
  f.drift = s.drift;
  return skywx::paint(buf, n, f, n != W || cloudsCoverBody(s), stop);
}

// ---------- what Home shows ----------

// The glance row under the date (display.html glance()): up to three live stats, each
// leading to its panel. Rotate highlights one, press or tap opens it.
struct Glance {
  String label, value, target;
  lv_color_t color;
};

std::vector<Glance> glance(uint8_t mask) {
  std::vector<Glance> out;
  if (mask & SHOW_TIMERS) {
    if (timers::focusActive())
      out.push_back({"Focus", timers::clockText(timers::focusLeft()), "focus", theme::accent()});
    const timers::Timer *soon = nullptr;
    for (const auto &t : timers::all())
      if (!soon || timers::left(t) < timers::left(*soon)) soon = &t;
    if (soon) out.push_back({soon->label, timers::clockText(timers::left(*soon)), "timers", theme::today()});
  }
  if ((mask & SHOW_HABITS) && !habits::all().empty())
    out.push_back({"Habits", String(habits::doneToday()) + "/" + habits::all().size(), "habits", theme::ink()});
  if (mask & SHOW_COUNTDOWN)
    for (const auto &c : countdowns::sorted()) {
      int32_t n = countdowns::daysUntil(c);
      if (n < 0) break;  // sorted: upcoming first
      out.push_back({c.name, n == 0 ? String("Today") : String(n) + "d", "countdowns", n == 0 ? theme::ok() : theme::ink()});
      break;
    }
  if (out.size() > 3) out.resize(3);
  return out;
}

// "82° Clear" + " • H 83 L 55": now and today only (2026-11-11: no "Rain Thu" or other
// future days on Home). False without weather data. Data from 3 hours ago or more (Wi-Fi down)
// says how old it is; from a day ago or more, "today" has run out, so only the last reading
// and its age are shown.
bool weatherLine(String &main, String &rest) {
  weather::Data d;
  if (!weather::get(d)) return false;
  const char *sep = " \xE2\x80\xA2 ";
  main = String(d.now.temp) + "\xC2\xB0 " + weather::condName(d.now.code);
  long age = (long)(time(nullptr) - d.fetched);
  if (age >= 24 * 3600) {
    rest = String(sep) + (age / 86400) + (age < 2 * 86400 ? " day old" : " days old");
    return true;
  }
  int d0 = d.firstDay();
  rest = age >= 3 * 3600 ? String(sep) + (age / 3600) + " h old" : String("");
  rest += String(sep) + "H " + d.days[d0].hi + " L " + d.days[d0].lo;
  return true;
}

// Secondary text color: on the bright sky the usual grey is hard to read.
// WCAG relative luminance and contrast, for stat colors over the Living sky (2026-11-10:
// the blue Focus value vanished into a blue sky).
float luminance(float r, float g, float b) {
  auto ch = [](float v) { v /= 255.0f; return v <= 0.03928f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); };
  return 0.2126f * ch(r) + 0.7152f * ch(g) + 0.0722f * ch(b);
}
float contrast(float la, float lb) { return (fmaxf(la, lb) + 0.05f) / (fminf(la, lb) + 0.05f); }

const Ink &inkFor(int f, bool night) { return night ? FACES[f].night : FACES[f].day; }
lv_color_t inkOr(uint32_t hex, lv_color_t fallback) { return hex ? lv_color_hex(hex) : fallback; }
lv_color_t dimFor(int f, bool night = false) { return inkOr(inkFor(f, night).dim, theme::inkDim()); }
bool nightNow(int f) { return FACES[f].body && sceneNow(f, clockx::local()).night; }
// Home's text changes color at sunrise/sunset only on faces with different night colors.
bool inkByTime(int f) { return memcmp(&FACES[f].day, &FACES[f].night, sizeof(Ink)) != 0; }

struct HomePanel;
Screen *makeCustomize(HomePanel *home);

struct HomePanel : Panel {
  BgCanvas bgc;
  lv_obj_t *bg = nullptr, *body = nullptr;
  lv_obj_t *clockLabel = nullptr, *dateLabel = nullptr, *stats = nullptr;
  lv_obj_t *wxRow = nullptr, *wxMain = nullptr, *wxRest = nullptr;  // "82° Clear • H 83 L 55"
  uint32_t wxVer = ~0u, testGen = 0;
  int shownMinute = -1, builtFace = -1;
  bool builtNight = false;
  lv_obj_t *clockShadow = nullptr;
  int sel = 0;
  String statsSig;
  std::vector<lv_obj_t *> statValues, statBars;
  std::vector<lv_color_t> statColors, shownColors;
  uint32_t lastStats = 0;
  int64_t wantKey = INT64_MIN;

  HomePanel() : Panel("home", "Home", &icon_home) {}

  void build(lv_obj_t *root) override {
    builtFace = face();
    bg = bgc.build(root);  // the last painted background, right away

    body = lv_image_create(root);  // sun or moon
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    if (!FACES[builtFace].body) lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *col = lv_obj_create(root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 400, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    const FaceDef &F = FACES[builtFace];
    builtNight = nightNow(builtFace);
    const Ink &ink = inkFor(builtFace, builtNight);
    lv_color_t dim = dimFor(builtFace, builtNight);
    const lv_font_t *clockFont = F.clockFont ? F.clockFont : &font_clock_120;
    clockShadow = nullptr;
    if (ink.shadow) {
      // A drop shadow: the shadow label takes the clock's place in the column and the clock sits
      // on it as a child, nudged up-left (labels can't draw their own shadow).
      clockShadow = ui::label(col, "", clockFont, lv_color_hex(ink.shadow));
      lv_obj_add_flag(clockShadow, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
      clockLabel = ui::label(clockShadow, "", clockFont, inkOr(ink.clock, theme::ink()));
      lv_obj_set_pos(clockLabel, -3, -5);
    } else {
      clockLabel = ui::label(col, "", clockFont, inkOr(ink.clock, theme::ink()));
    }
    dateLabel = ui::label(col, "", &lv_font_montserrat_24, inkOr(ink.date, dim));
    if (F.upper) lv_obj_set_style_text_letter_space(dateLabel, 2, 0);
    wxRow = lv_obj_create(col);
    lv_obj_remove_style_all(wxRow);
    lv_obj_set_size(wxRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wxRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_top(wxRow, 6, 0);
    lv_obj_remove_flag(wxRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wxRow, LV_OBJ_FLAG_CLICKABLE);
    wxMain = ui::label(wxRow, "", &lv_font_montserrat_20, inkOr(ink.wx, theme::ink()));
    wxRest = ui::label(wxRow, "", &lv_font_montserrat_20, inkOr(ink.wx, dim));
    wxVer = ~0u;
    stats = lv_obj_create(col);
    lv_obj_remove_style_all(stats);
    lv_obj_set_size(stats, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(stats, 14, 0);
    lv_obj_set_style_pad_top(stats, 18, 0);
    lv_obj_remove_flag(stats, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(stats, LV_OBJ_FLAG_CLICKABLE);

    shownMinute = -1;
    statsSig = "";
    tick();
  }

  void buildStats(const std::vector<Glance> &g) {
    lv_obj_clean(stats);
    statValues.clear();
    statColors.clear();
    shownColors.clear();
    statBars.clear();
    sel = constrain(sel, 0, max(0, (int)g.size() - 1));
    lv_color_t dim = dimFor(builtFace, builtNight);
    for (int i = 0; i < (int)g.size(); i++) {
      lv_obj_t *box = lv_obj_create(stats);
      lv_obj_remove_style_all(box);
      // Sized to its text like display.html's .stat (min-width 17%, 6 px sides), not a fixed 110
      // px: three stats sat 125 px apart (2026-11-10).
      lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_style_min_width(box, 82, 0);
      lv_obj_set_style_pad_hor(box, 6, 0);
      lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_bottom(box, 10, 0);
      lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_user_data(box, (void *)(intptr_t)i);
      lv_obj_add_event_cb(box, [](lv_event_t *e) {
        auto *self = (HomePanel *)lv_event_get_user_data(e);
        self->sel = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
        self->press();
      }, LV_EVENT_CLICKED, this);
      statValues.push_back(ui::label(box, g[i].value.c_str(), &lv_font_montserrat_36, readable(g[i].color)));
      statColors.push_back(g[i].color);
      lv_obj_t *l = ui::label(box, ui::upper(g[i].label.c_str()), &lv_font_montserrat_14, dim);
      lv_obj_set_style_text_letter_space(l, 2, 0);
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_size(l, LV_SIZE_CONTENT, lv_font_get_line_height(&lv_font_montserrat_14));
      lv_obj_set_style_max_width(l, 106, 0);  // a long countdown name ends in "..."
      // ".stat.sel::after": a glowing accent bar under the highlighted stat.
      lv_obj_t *bar = lv_obj_create(box);
      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, 62, 3);
      lv_obj_set_style_radius(bar, 2, 0);
      lv_obj_set_style_bg_color(bar, FACES[builtFace].lightBg ? lv_color_white() : theme::accent(), 0);  // accent on a blue sky: gone
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_set_style_shadow_color(bar, FACES[builtFace].lightBg ? lv_color_white() : theme::accent(), 0);
      lv_obj_set_style_shadow_width(bar, 12, 0);
      lv_obj_set_style_margin_top(bar, 6, 0);
      lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
      statBars.push_back(bar);
    }
    showSel();
  }

  void showSel() {
    for (int i = 0; i < (int)statBars.size(); i++) lv_obj_set_style_opa(statBars[i], i == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  }

  void updateStats() {
    std::vector<Glance> g = glance(shown());
    String sig;
    for (auto &x : g) sig += x.target + x.label + "|";
    if (sig != statsSig) {
      statsSig = sig;
      buildStats(g);
      return;
    }
    for (int i = 0; i < (int)g.size(); i++) {
      lv_label_set_text(statValues[i], g[i].value.c_str());
      // The sky changes through the day: recheck the color (restyle only when it changes).
      lv_color_t c = readable(g[i].color);
      if (i >= (int)shownColors.size()) shownColors.resize(i + 1, lv_color_black());
      if (!lv_color_eq(c, shownColors[i])) {
        shownColors[i] = c;
        lv_obj_set_style_text_color(statValues[i], c, 0);
      }
    }
  }

  // A stat's own color (accent for Focus, amber for a timer) unless it's too close to the sky
  // behind it on a light face like Living sky (contrast under 3:1, the large-text minimum); then white like the clock,
  // or the screen's near-black on a very light sky.
  lv_color_t readable(lv_color_t c) {
    if (!FACES[builtFace].lightBg) return c;
    Scene s = sceneNow(builtFace, clockx::local());
    RGB bg = baseAt(FACES[builtFace], rgb(SCREEN_HEX), s.sky.topAt(s.hour), s.sky.at(s.hour), 345.0f);  // the stat values' row
    float lb = luminance(bg.r, bg.g, bg.b);
    if (contrast(luminance(c.red, c.green, c.blue), lb) >= 3.0f) return c;
    // White like the clock and the other stats, unless the sky is so light (a sunrise horizon)
    // that white itself fades (under 2:1); then the screen's near-black.
    return contrast(1.0f, lb) >= 2.0f ? lv_color_white() : theme::screen();
  }

  // Weather line under the date (display.html renderHome): only with weather data, and only
  // when it's turned on.
  void updateWeather() {
    wxVer = weather::version();
    String main, rest;
    if (!shows(SHOW_WEATHER) || !weatherLine(main, rest)) {
      lv_obj_add_flag(wxRow, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    lv_obj_remove_flag(wxRow, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wxMain, main.c_str());
    lv_label_set_text(wxRest, rest.c_str());
  }

  void rotate(int dir) override {
    int n = constrain(sel + dir, 0, max(0, (int)statBars.size() - 1));
    if (n != sel) { sel = n; showSel(); }
  }

  void press() override {
    std::vector<Glance> g = glance(shown());
    if (sel < (int)g.size()) app::goTo(g[sel].target.c_str());
  }

  // Swipe up from the bottom edge: customize Home.
  bool swipeVertical(int dir, int startY) override {
    if (dir < 0 && startY >= 400) { app::push(makeCustomize(this)); return true; }
    return false;
  }

  // Ask the painter for this minute's background (for the chosen face). Also called while the
  // customize sheet is up, so a newly chosen face is painted before Home comes back.
  void requestBackground(const struct tm &now) {
    int f = face();
    float lat, lon;
    if (!weather::coords(lat, lon)) tz::fallbackCoords(lat, lon);
    int64_t key = ((int64_t)(now.tm_year * 366 + now.tm_yday) * 1440 + now.tm_hour * 60 + now.tm_min) * 1000003LL +
                  (int64_t)lroundf(lat * 100) * 40000 + lroundf(lon * 100) + (int64_t)f * 7919;
    Scene s = sceneNow(f, now);
    key = key * 131 + s.wx;  // a weather face repaints when the weather changes
    if (!FACES[f].body) key = -7 - f;  // doesn't change with time
    if (key == wantKey) return;
    wantKey = key;
    int hh = now.tm_hour, mm = now.tm_min;
    bgc.request(key, [s, hh, mm](uint16_t *buf) {
      uint32_t t0 = millis();
      paintScene(buf, W, s);
      Serial.printf("[home] %02d:%02d %s %s, weather %d, background %lu ms\n", hh, mm, FACES[s.face].name,
                    s.night ? "night" : "day", FACES[s.face].weather ? s.wx : -1, (unsigned long)(millis() - t0));
    });
  }

  void background() override {
    if (app::currentPanel() != this) return;  // only while the customize sheet is over Home
    bgc.tick();
    requestBackground(clockx::local());
  }

  void tick() override {
    bgc.tick();
    if (millis() - lastStats >= 250) {
      lastStats = millis();
      updateStats();
    }
    struct tm now = clockx::local();
    if (weather::version() != wxVer || skyTestGen != testGen) {
      testGen = skyTestGen;
      updateWeather();
      shownMinute = -1;  // new coordinates move the sunrise/sunset; new weather changes Living sky
    }
    // The minute, plus what else changes the text: the hour (a new time zone) and the formats.
    int shown = (((now.tm_mday * 24 + now.tm_hour) * 60 + now.tm_min) * 4 + tz::h24() * 2 + tz::dayFirst()) * 2 + clockx::trusted();
    if (shown == shownMinute) return;
    shownMinute = shown;
    if (now.tm_min == 0) updateWeather();  // today's H/L changes at midnight; refreshes the "h old" age

    const FaceDef &F = FACES[builtFace];
    if (F.body && inkByTime(builtFace) && sceneNow(builtFace, now).night != builtNight) {
      app::refresh();  // sunrise or sunset on a face with night colors: rebuild with them
      return;
    }
    String clock = tz::clockText(now.tm_hour, now.tm_min);
    lv_label_set_text(clockLabel, clock.c_str());
    if (clockShadow) lv_label_set_text(clockShadow, clock.c_str());
    // A clock still on the firmware's build time says so instead of showing a wrong date.
    String date = !clockx::trusted()
                      ? String(net::configured() && net::autoTime() ? "Waiting for network time" : "Clock not set")
                      : tz::dateText(daynum::dayLong(now.tm_wday), daynum::monthShort(now.tm_mon + 1), now.tm_mday);
    lv_label_set_text(dateLabel, F.upper ? ui::upper(date.c_str()) : date.c_str());

    if (F.body) {
      Scene s = sceneNow(builtFace, now);
      // Heat paints its own sun; under clouds Living sky paints the sun or moon dimmed.
      lv_obj_set_flag(body, LV_OBJ_FLAG_HIDDEN, (!s.night && !F.daySprite) || cloudsCoverBody(s));
      const lv_image_dsc_t *spr = &sprite_sun;
      if (s.night) {
        spr = moon::sprite(FACES[builtFace].moonR, FACES[builtFace].moonHalo);
        if (!spr) spr = &sprite_moon;  // no PSRAM for it: the old crescent
      }
      lv_image_set_src(body, spr);
      lv_obj_invalidate(body);  // the moon sprite is redrawn in place as the phase moves
      lv_obj_set_pos(body, (int32_t)lroundf(s.bx) - spr->header.w / 2, (int32_t)lroundf(s.by) - spr->header.h / 2);
    }
    requestBackground(now);  // one background per minute, location and face; rebuilding Home reuses it
  }
};

// ---------- customize sheet (home-customize-mockups.html) ----------
// A carousel of face previews: the middle one is Home as it will look, with your elements; the
// neighbours peek in at the sides. Swipe it (or turn the knob) to change the face; tap a side
// preview to bring it to the middle; tap the middle one, press, swipe down or hold to close.
// Below, four element toggles. The knob walks faces then toggles in one line; press on a face
// picks it and closes, press on a toggle flips it.
//
// Previews: a face's background painted at 240 px (middle) and 138 px (sides) on core 0, on
// demand: only the faces on screen (the middle one first) get painted, into a few reusable
// slots of ~150 KB each, so memory stays flat however many faces there are. A face not painted
// yet shows an empty circle for the moment it takes (~50-150 ms). Text on top is live LVGL
// labels in small fonts, laid out like Home, so turning an element off rearranges the preview.
constexpr int PV = 240;            // middle preview size
constexpr int PV_TOP = 48;         // its top edge
constexpr int SPACING = 192;       // between preview centers
constexpr int SIDE = 138;          // side preview size (painted at this size: LVGL image scaling is slow)
constexpr int DOTS_Y = 302, TOGGLES_Y = 322;

// One face's previews at both sizes. face and ready change only under Thumbs::m (face is also
// read without it, to abandon a stale paint).
struct Slot {
  uint16_t *big = nullptr, *small = nullptr;
  std::atomic<int> face{-1};  // the face it holds or is being painted for; -1 free
  bool ready = false;         // painted
  uint32_t used = 0;          // when it was last wanted (a counter): the oldest is reused first
};

// Three faces are on screen at most; the fourth slot holds the next one in the direction you're
// scrolling, painted ahead so it's ready when it slides in.
constexpr int MAX_SLOTS = 4;

struct Thumbs {
  Slot slot[MAX_SLOTS];
  int slots = 0;
  uint32_t useClock = 0;
  std::mutex m;
  std::atomic<int> cur{0};          // the middle face: painted first
  std::atomic<uint32_t> ready{0};   // bit per face: preview painted, or none needed
  std::atomic<bool> cancel{false}, running{false};
  std::vector<Scene> scene;         // one per face, filled before the painter task starts

  Slot *find(int f) {  // under m
    for (int i = 0; i < slots; i++)
      if (slot[i].face == f) return &slot[i];
    return nullptr;
  }

  // The painter (core 0): paints whichever wanted slot is closest to the middle, until closed.
  static void task(void *arg) {
    Thumbs *t = (Thumbs *)arg;
    while (!t->cancel) {
      Slot *s = nullptr;
      int f = -1;
      {
        std::lock_guard<std::mutex> l(t->m);
        for (int i = 0; i < t->slots; i++) {
          Slot &c = t->slot[i];
          if (c.face < 0 || c.ready) continue;
          if (!s || abs(c.face - t->cur) < abs(f - t->cur)) { s = &c; f = c.face; }
        }
      }
      if (!s) { vTaskDelay(pdMS_TO_TICKS(15)); continue; }
      auto stale = [t, s, f] { return t->cancel || s->face != f; };
      uint32_t t0 = millis();
      bool done = paintScene(s->big, PV, t->scene[f], stale) && paintScene(s->small, SIDE, t->scene[f], stale);
      std::lock_guard<std::mutex> l(t->m);
      if (done && s->face == f) {
        s->ready = true;
        t->ready |= 1u << f;
        Serial.printf("[home] preview %s in %lu ms\n", FACES[f].name, (unsigned long)(millis() - t0));
      }
    }
    t->running = false;
    vTaskDelete(nullptr);
  }
};
static_assert(FACE_COUNT < 32, "Thumbs::ready has one bit per face");

const struct { uint8_t bit; const char *name; const lv_image_dsc_t *icon; } TOGGLES[4] = {
  {SHOW_WEATHER, "Weather", &icon_weather}, {SHOW_HABITS, "Habits", &icon_habits},
  {SHOW_COUNTDOWN, "Days", &icon_countdowns}, {SHOW_TIMERS, "Timers", &icon_timers},
};

struct Customize : Screen {
  HomePanel *home;
  Thumbs *th;
  int cur;                 // face in the middle
  bool zoneToggles = false;
  int tsel = 0;
  // The blue selection shows only while the knob is in use (it followed the finger). A
  // tap or swipe hides it; the first turn or press after that just shows it again.
  bool knob = false;
  lv_obj_t *strip = nullptr, *dots = nullptr;
  lv_obj_t *canvases[FACE_COUNT] = {};
  lv_image_dsc_t dsc[FACE_COUNT], sdsc[FACE_COUNT];
  std::vector<lv_obj_t *> toggleBtns;
  uint32_t shownReady = 0;
  int slideFrom = 0;       // -1/+1: the strip slides in from that side on the next build
  int heading = 1;         // the last scroll direction: the face beyond the side preview is painted ahead

  explicit Customize(HomePanel *h) : home(h), th(new Thumbs()), cur(face()) {
    // The previews need ~300 KB; the Focus dial's kept image would leave too little after a
    // visit to Focus (Living sky's preview went missing). No dial is on screen here.
    Dial::releaseCache();
    struct tm now = clockx::local();
    int bodies = 0;
    for (int f = 0; f < FACE_COUNT; f++) {
      th->scene.push_back(sceneNow(f, now));
      if (FACES[f].body) bodies++;
      else th->ready |= 1u << f;  // flat: nothing to paint, the placeholder is the preview
    }
    for (int i = 0; i < min(bodies, MAX_SLOTS); i++) {
      Slot &s = th->slot[i];
      s.big = (uint16_t *)heap_caps_malloc(PV * PV * 2, MALLOC_CAP_SPIRAM);
      s.small = (uint16_t *)heap_caps_malloc(SIDE * SIDE * 2, MALLOC_CAP_SPIRAM);
      if (!s.big || !s.small) {  // both or neither; fewer slots just means some placeholders
        heap_caps_free(s.big);
        heap_caps_free(s.small);
        s.big = s.small = nullptr;
        Serial.printf("[home] room for %d preview slots, not %d (PSRAM free %u KB)\n", i, min(bodies, MAX_SLOTS),
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        break;
      }
      th->slots++;
    }
    th->running = true;
    if (xTaskCreatePinnedToCore(Thumbs::task, "thumbs", 4096, th, 1, nullptr, 0) != pdPASS) {
      // No memory for the task: no previews rather than a hang on close.
      th->running = false;
      th->slots = 0;
      th->ready = (1u << FACE_COUNT) - 1;
      Serial.println("[home] no room for the preview painter");
    }
  }

  ~Customize() override {
    th->cancel = true;
    while (th->running) delay(1);  // the painter abandons its paint within 8 rows and stops
    for (auto &s : th->slot) {
      heap_caps_free(s.big);
      heap_caps_free(s.small);
    }
    delete th;
  }

  void build(lv_obj_t *root) override {
    toggleBtns.clear();
    for (auto &c : canvases) c = nullptr;
    strip = lv_obj_create(root);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, W, PV + 8);
    lv_obj_set_pos(strip, 0, PV_TOP - 4);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    wantPreviews();
    // Sides first so the middle one draws on top.
    for (int d : {-1, 1, 0}) {
      int f = cur + d;
      if (f < 0 || f >= FACE_COUNT) continue;
      buildPreview(f, d);
    }
    shownReady = th->ready;

    dots = ui::row(root, 8);
    lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, DOTS_Y);
    for (int f = 0; f < FACE_COUNT; f++) {
      lv_obj_t *d = lv_obj_create(dots);
      lv_obj_remove_style_all(d);
      lv_obj_set_size(d, 8, 8);
      lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(d, f == cur ? theme::ink() : theme::inkFaint(), 0);
      lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }

    lv_obj_t *row = ui::row(root, 14);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);  // one line, never wrapped
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, TOGGLES_Y);
    for (int i = 0; i < 4; i++) {
      bool on = shown() & TOGGLES[i].bit;
      lv_obj_t *cell = lv_obj_create(row);
      lv_obj_remove_style_all(cell);
      lv_obj_set_size(cell, 78, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(cell, 5, 0);
      lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_user_data(cell, (void *)(intptr_t)i);
      lv_obj_add_event_cb(cell, [](lv_event_t *e) {
        auto *self = (Customize *)lv_event_get_user_data(e);
        int i = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
        self->zoneToggles = true;  // a later turn carries on from here
        self->tsel = i;
        self->knob = false;
        self->flip(i);
      }, LV_EVENT_CLICKED, this);
      lv_obj_t *b = lv_obj_create(cell);
      lv_obj_remove_style_all(b);
      lv_obj_set_size(b, 58, 58);
      lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(b, theme::accent(), 0);
      lv_obj_set_style_bg_opa(b, on ? LV_OPA_20 : LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(b, 1, 0);
      lv_obj_set_style_border_color(b, on ? theme::accent() : theme::inkFaint(), 0);
      lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
      bool sel = knob && zoneToggles && tsel == i;
      if (sel) {
        lv_obj_set_style_outline_width(b, 2, 0);
        lv_obj_set_style_outline_color(b, theme::accent(), 0);
        lv_obj_set_style_shadow_color(b, theme::accent(), 0);
        lv_obj_set_style_shadow_width(b, 14, 0);
        lv_obj_set_style_shadow_opa(b, LV_OPA_50, 0);
      }
      lv_obj_t *img = lv_image_create(b);
      lv_image_set_src(img, TOGGLES[i].icon);
      lv_obj_set_style_image_recolor(img, on ? lv_color_white() : theme::inkDim(), 0);
      lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
      lv_obj_center(img);
      ui::label(cell, TOGGLES[i].name, &lv_font_montserrat_14, on ? theme::ink() : theme::inkDim());
      toggleBtns.push_back(cell);
    }
    ui::roomForGlow(row);

    if (slideFrom) {
      // Slide the previews in from the side they came from (one container: cheap to move).
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, strip);
      lv_anim_set_values(&a, slideFrom * SPACING, 0);
      lv_anim_set_duration(&a, 220);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t *)o, v, 0); });
      lv_anim_start(&a);
      slideFrom = 0;
    }
  }

  // Give each face on screen a slot (the painter fills them, the middle one first), then the
  // next face in the scroll direction, reusing the slot wanted longest ago. With fewer slots
  // than that (low memory), the rest keep the placeholder.
  void wantPreviews() {
    std::lock_guard<std::mutex> l(th->m);
    th->cur = cur;
    for (int d : {0, heading, -heading, 2 * heading}) {
      int f = cur + d;
      if (f < 0 || f >= FACE_COUNT || !FACES[f].body) continue;
      Slot *s = th->find(f);
      if (!s) {
        for (int i = 0; i < th->slots; i++) {
          Slot &c = th->slot[i];
          if (c.face >= 0 && abs(c.face - cur) <= 1) continue;  // on screen now
          if (c.face >= 0 && c.face == cur + 2 * heading) continue;
          if (!s || c.used < s->used) s = &c;
        }
        if (!s) continue;
        if (s->face >= 0) th->ready &= ~(1u << s->face);
        s->face = f;
        s->ready = false;
      }
      s->used = ++th->useClock;
    }
  }

  // One preview: the face's background (or a placeholder until it's painted) plus Home's
  // text, scaled down. d = -1/0/+1: left, middle, right.
  void buildPreview(int f, int d) {
    bool mid = d == 0;
    int size = mid ? PV : SIDE;
    int cx = W / 2 + d * SPACING;
    lv_obj_t *box = lv_obj_create(strip);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_set_pos(box, cx - size / 2, 4);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(box, (void *)(intptr_t)f);
    lv_obj_add_event_cb(box, [](lv_event_t *e) {
      auto *self = (Customize *)lv_event_get_user_data(e);
      int f = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
      if (f == self->cur) { app::pop(); return; }  // the middle one again: "this one", close
      self->knob = false;
      self->zoneToggles = false;
      self->choose(f);
    }, LV_EVENT_CLICKED, this);

    uint16_t *px = nullptr;
    {
      std::lock_guard<std::mutex> l(th->m);
      Slot *s = th->find(f);
      if (s && s->ready) px = mid ? s->big : s->small;
    }
    if (px) {
      lv_image_dsc_t &ds = mid ? dsc[f] : sdsc[f];
      memset(&ds, 0, sizeof(ds));
      ds.header.magic = LV_IMAGE_HEADER_MAGIC;
      ds.header.cf = LV_COLOR_FORMAT_RGB565;
      ds.header.w = size;
      ds.header.h = size;
      ds.header.stride = size * 2;
      ds.data_size = size * size * 2;
      ds.data = (const uint8_t *)px;
      lv_obj_t *img = lv_image_create(box);
      lv_image_set_src(img, &ds);
      lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
      if (!mid) lv_obj_set_style_image_opa(img, LV_OPA_60, 0);
      canvases[f] = img;
    }
    // The circle's edge, over the image: a faint ring, accent when the knob is on the faces.
    lv_obj_t *edge = lv_obj_create(box);
    lv_obj_remove_style_all(edge);
    lv_obj_set_size(edge, size, size);
    lv_obj_set_style_radius(edge, LV_RADIUS_CIRCLE, 0);
    bool sel = knob && mid && !zoneToggles;
    lv_obj_set_style_border_width(edge, sel ? 4 : 2, 0);
    lv_obj_set_style_border_color(edge, sel ? theme::accent() : lv_color_white(), 0);
    lv_obj_set_style_border_opa(edge, sel ? LV_OPA_COVER : LV_OPA_20, 0);
    // (No glow: a blurred shadow this size costs ~100 ms per redraw.)
    lv_obj_remove_flag(edge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(edge, LV_OBJ_FLAG_SCROLLABLE);

    // Home's text, laid out like Home (a centered column), in small fonts.
    lv_obj_t *col = lv_obj_create(box);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, size, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, mid ? 3 : 2);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    bool night = th->scene[f].night;
    const Ink &ink = inkFor(f, night);
    lv_color_t dim = dimFor(f, night);
    lv_opa_t opa = mid ? LV_OPA_COVER : LV_OPA_60;
    struct tm now = clockx::local();
    lv_obj_t *c = ui::label(col, tz::clockText(now.tm_hour, now.tm_min).c_str(), mid ? &lv_font_montserrat_48 : &lv_font_montserrat_28, inkOr(ink.clock, theme::ink()));
    lv_obj_set_style_text_opa(c, opa, 0);
    if (!mid) return;
    String date = tz::dateText(daynum::dayLong(now.tm_wday), daynum::monthShort(now.tm_mon + 1), now.tm_mday);
    ui::label(col, FACES[f].upper ? ui::upper(date.c_str()) : date.c_str(), &lv_font_montserrat_12, inkOr(ink.date, dim));
    String main, rest;
    if (shows(SHOW_WEATHER) && weatherLine(main, rest)) {
      String line = main + rest;
      lv_obj_t *wx = ui::label(col, line.c_str(), &lv_font_montserrat_10, inkOr(ink.wx, dim));
      lv_obj_set_style_pad_top(wx, 3, 0);
    }
    std::vector<Glance> g = glance(shown());
    if (!g.empty()) {
      lv_obj_t *st = ui::row(col, 8);
      lv_obj_set_size(st, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
      lv_obj_set_style_pad_top(st, 7, 0);
      for (auto &x : g) {
        lv_obj_t *cell = lv_obj_create(st);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 50, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        ui::label(cell, x.value.c_str(), &lv_font_montserrat_18, x.color);
        lv_obj_t *l = ui::label(cell, ui::upper(x.label.c_str()), &lv_font_montserrat_10, dim);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, 50);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      }
    }
  }

  void choose(int f) {
    f = constrain(f, 0, FACE_COUNT - 1);
    if (f == cur) return;
    slideFrom = heading = f > cur ? 1 : -1;
    cur = f;
    store::setInt("homeface", f);  // Home's background starts painting behind the sheet
    app::refresh();
  }

  void flip(int i) {
    store::setInt("homeshow", shown() ^ TOGGLES[i].bit);
    app::refresh();
  }

  // The knob walks one line: the faces in FACES order, then Weather, Habits, Days, Timers.
  // Press on a face picks it and closes; press on a toggle flips it. The blue selection
  // shows only once the knob is used (see knob).
  void rotate(int dir) override {
    if (!knob) { knob = true; app::refresh(); return; }  // first turn: show where the knob is
    if (!zoneToggles) {
      if (dir > 0 && cur == FACE_COUNT - 1) { zoneToggles = true; tsel = 0; app::refresh(); return; }
      choose(cur + dir);
      return;
    }
    if (dir < 0 && tsel == 0) { zoneToggles = false; app::refresh(); return; }
    int n = constrain(tsel + dir, 0, 3);
    if (n != tsel) { tsel = n; app::refresh(); }
  }

  void press() override {
    if (!zoneToggles) { app::pop(); return; }
    if (!knob) { knob = true; app::refresh(); return; }  // don't flip a toggle you can't see selected
    flip(tsel);
  }

  bool swipe(int dir) override {
    knob = false;
    choose(cur + dir);
    return true;
  }

  bool swipeVertical(int dir, int) override {
    if (dir > 0) { app::pop(); return true; }  // swipe down closes
    return false;
  }

  void tick() override {
    // A preview finished painting: show it.
    if (th->ready != shownReady) app::refresh();
  }
};

Screen *makeCustomize(HomePanel *home) { return new Customize(home); }

}  // namespace

Panel *makeHomePanel() { return new HomePanel(); }

// Serial X<code>: show that weather on Living sky (X- goes back to the real weather).
void homeTestSkyCode(int code) {
  skyTestCode = code;
  skyTestGen++;
  Serial.printf("[home] sky weather %d%s\n", code >= 0 ? code : skyCode(), code >= 0 ? " (test)" : "");
}
