#include "tray.h"

#include <Arduino.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include <vector>

#include "app.h"
#include "board.h"
#include "dither.h"
#include "panels.h"
#include "tray_assets.h"

namespace tray {
namespace {

constexpr int S = 480;

// ---------- layout (fasttravel-mockups.html option D, scaled from 340 to 480 px) ----------
constexpr int EDGE = 77;             // a pull starts in the top 16% of the screen
constexpr float PULL = 0.34f * S;    // finger travel for a fully open sheet
constexpr float ICON_Y = 0.43f * S;  // icon row centre
constexpr float SP = 0.25f * S;      // spacing between neighbours
constexpr int NAME_Y = 317;          // name centre (66%)
constexpr int DOTS_Y = 374;          // position dots (78%)
constexpr float DOT_R = 4.2f, DOT_SEL_R = 5.6f, DOT_GAP = 7;
constexpr int HANDLE_Y = 452, HANDLE_W = 58, HANDLE_H = 6;
// Quick buttons along the top (brightness-tray-mockups.html option D; more may join it).
constexpr int QUICK_Y = 65;          // centre (13.5%)
constexpr float QUICK_R = 29;        // 12% of the screen across
constexpr float QUICK_HIT = 49;      // tap radius: 25% past the old 39 (2026-11-10); the disc stays 29
constexpr int SHADOW = 22;           // soft shadow under the sheet's edge
constexpr float TAU_POS = 55;        // ms; the lists' glide
constexpr float TAU_SHEET = 40;      // ms; the sheet settling open or shut
constexpr float THROW_MS = 220;      // flick velocity x this = how far it coasts (as the lists)
// Plus a part that grows with the square of the speed, so a firm flick (~1.5 px/ms) crosses
// ~5 panels and a hard one most of the row, while a gentle one (~0.6 px/ms) moves ~1.5.
// Releases slower than THROW_MIN don't coast at all, so small moves land where they stop.
// (one good flick should get through most of the menu; 23680 overshot small moves.)
constexpr float THROW_BOOST = 17000;
constexpr float THROW_MIN = 0.003f;  // items/ms (~0.35 px/ms)
constexpr int SLOP = 10;
constexpr uint32_t HOLD_MS = 700;
constexpr uint32_t SETTLE_MS = 5000;  // left open and untouched this long: go to the panel it rests on

// Rows each part of the sheet covers (for partial redraws while the row glides).
constexpr int BAND_Y0 = (int)ICON_Y - (TRAY_ICON / 2 + TRAY_GLOW_PAD) - 2;
constexpr int BAND_Y1 = (int)ICON_Y + (TRAY_ICON / 2 + TRAY_GLOW_PAD) + 2;
constexpr int INFO_Y0 = NAME_Y - TRAY_NAME_H / 2 - 2;
constexpr int INFO_Y1 = DOTS_Y + 8;

const uint32_t INK = 0xF2F4F8, ACCENT = 0x4F8CFF;

inline uint16_t c565(uint32_t hex) {
  return (uint16_t)(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F));
}
// a: 0..255
inline uint16_t blend(uint16_t d, uint16_t s, int a) {
  uint32_t fg = (s | ((uint32_t)s << 16)) & 0x07E0F81F;
  uint32_t bg = (d | ((uint32_t)d << 16)) & 0x07E0F81F;
  uint32_t r = ((((fg - bg) * (uint32_t)((a + 4) >> 3)) >> 5) + bg) & 0x07E0F81F;
  return (uint16_t)(r | (r >> 16));
}

// ---------- masks ----------
struct Mask {
  const uint8_t *px = nullptr;
  int w = 0, h = 0;
  std::vector<uint8_t> span;  // per row: first and one-past-last non-zero column
};
struct Item {
  Mask icon, glow;
  const uint8_t *name;
  int nameW;
};
Item items[16];
const char *ids[16];
int nItems = 0;

void makeSpans(Mask &m) {
  m.span.assign(m.h * 2, 0);
  for (int y = 0; y < m.h; y++) {
    const uint8_t *r = m.px + y * m.w;
    int a = 0, b = m.w;
    while (a < m.w && !r[a]) a++;
    while (b > a && !r[b - 1]) b--;
    m.span[y * 2] = a;
    m.span[y * 2 + 1] = b;
  }
}

// ---------- buffers ----------
// PSRAM is nearly full after boot (~290 KB free), so the tray keeps no full-screen buffers:
// the sheet's background is generated row by row from small tables, and rows the sheet
// uncovers are redrawn by LVGL. The strip it composes in is borrowed only while it's open
// (internal RAM is tight too: the wheel needs a 46 KB block while spinning).
constexpr int STRIP = 16;
uint16_t *strip = nullptr;
// Background: radial-gradient(circle at 50% 30%, #1a2030, #0d1016 72%), as in the mock-up.
// BG_LEVELS steps of the gradient, each pre-dithered for the 8x8 Bayer cell.
constexpr int BG_LEVELS = 64;
constexpr int BG_CX = 240, BG_CY = 144;
constexpr int BG_SHIFT = 6;   // distance squared >> 6 indexes the level table
uint16_t *bgPal = nullptr;    // BG_LEVELS x 64
uint8_t *bgLevel = nullptr;   // (max distance^2 >> BG_SHIFT) + 1 entries
int bgLevelN = 0;
bool bgReady = false;
// Fast copies of the tables in internal RAM while the tray is open (PSRAM lookups were the
// background's main cost).
uint16_t *palFast = nullptr;
uint8_t *levelFast = nullptr;
// The rows under the shadow, as they were before darkening, so a sheet moving down never
// needs LVGL (only rows it uncovers going up do).
uint16_t *saved = nullptr;
int savedY0 = 0, savedN = 0;

// ---------- state ----------
enum class Finger { None, Pull, Open };
bool isLive = false;
bool pendingOpen = false;
float reveal = 0, revealTo = 0;
float pos = 0, target = 0;
int go = -1;
std::vector<Panel *> list;
int here = 0;
uint32_t lastActivity = 0;  // finger, knob or button, for the 5 s auto-select

struct {
  Finger mode = Finger::None;
  int x0 = 0, y0 = 0, lx = 0, ly = 0;
  uint32_t downAt = 0;
  char axis = 0;     // 'x', 'y' or 0
  bool hdrag = false;
  int hx0 = 0;
  float hp0 = 0;
  bool moved = false;
  struct { uint32_t t; int x; } samples[12];
  int ns = 0;
} f;

// Candidate pull, tracked from LVGL's samples before the tray takes over.
bool cand = false;
int candX = 0, candY = 0;
bool wasDown = false;

// What's on screen now.
int drawnOff = 0;
float drawnPos = -99;
int drawnNear = -1;
bool underDirty = false;

// Frame timing.
uint32_t lastT = 0;
uint32_t perfFrames = 0, perfUs = 0, perfMax = 0, perfOpenAt = 0;
// Per kind of frame: sheet moving / row gliding; LVGL row redraws; the time between frames.
uint32_t pcBg = 0, pcGlow = 0, pcIcon = 0, pcRest = 0, pcWrite = 0;
uint32_t pfSheetN = 0, pfSheetUs = 0, pfRowN = 0, pfRowUs = 0, pfLvglUs = 0, pfGapMax = 0, pfLastEnd = 0;

// Scripted finger for serial tests.
struct {
  bool on = false;
  uint32_t t0 = 0;
  int items = 0;
} script;

// ---------- layout math (same as the mock-up) ----------
float itemX(float d) {
  float ad = fabsf(d);
  float m = fminf(ad, 2.0f);
  return copysignf(SP * (ad - 0.05f * m * m), d);
}
float itemK(float ad) { return fmaxf(0.38f, 1.0f - 0.36f * ad); }
float itemOp(float ad) { return fminf(1.0f, fmaxf(0.0f, 1.15f - 0.3f * ad)); }
int nearest() { return std::min(std::max((int)lroundf(pos), 0), nItems - 1); }

// ---------- drawing into the strip ----------
// The strip holds screen rows [sy0, sy1). Sheet coordinates are screen + shift.

void drawMask(const Mask &m, float cx, float cy, float scale, uint16_t color, int alpha, int sy0, int sy1) {
  if (alpha <= 0) return;
  float w = m.w * scale, h = m.h * scale;
  float left = cx - w / 2, top = cy - h / 2;
  int y0 = std::max(sy0, (int)floorf(top)), y1 = std::min(sy1, (int)ceilf(top + h));
  int bx0 = std::max(0, (int)floorf(left)), bx1 = std::min(S, (int)ceilf(left + w));
  if (y0 >= y1 || bx0 >= bx1) return;
  const float inv = 1.0f / scale;
  const int32_t step = (int32_t)(inv * 65536.0f);
  for (int y = y0; y < y1; y++) {
    int32_t my = (int32_t)floorf(((y + 0.5f - top) * inv - 0.5f) * 65536.0f);
    int iy = my >> 16;
    int fy = (my >> 8) & 255;
    const uint8_t *r0 = (iy >= 0 && iy < m.h) ? m.px + iy * m.w : nullptr;
    const uint8_t *r1 = (iy + 1 >= 0 && iy + 1 < m.h) ? m.px + (iy + 1) * m.w : nullptr;
    int s0 = m.w, s1 = 0;
    if (r0) { s0 = std::min(s0, (int)m.span[iy * 2]); s1 = std::max(s1, (int)m.span[iy * 2 + 1]); }
    if (r1) { s0 = std::min(s0, (int)m.span[(iy + 1) * 2]); s1 = std::max(s1, (int)m.span[(iy + 1) * 2 + 1]); }
    if (s0 >= s1) continue;
    // Mask columns [s0-1, s1] can contribute; turn that into screen columns.
    int x0 = std::max(bx0, (int)floorf(left + (s0 - 1 + 0.5f) * scale - 0.5f));
    int x1 = std::min(bx1, (int)ceilf(left + (s1 + 0.5f) * scale + 0.5f));
    uint16_t *dst = strip + (y - sy0) * S;
    int32_t mx = (int32_t)floorf(((x0 + 0.5f - left) * inv - 0.5f) * 65536.0f);
    for (int x = x0; x < x1; x++, mx += step) {
      int ix = mx >> 16;
      int fx = (mx >> 8) & 255;
      int a00 = 0, a01 = 0, a10 = 0, a11 = 0;
      bool in0 = ix >= 0 && ix < m.w, in1 = ix + 1 >= 0 && ix + 1 < m.w;
      if (r0) { if (in0) a00 = r0[ix]; if (in1) a01 = r0[ix + 1]; }
      if (r1) { if (in0) a10 = r1[ix]; if (in1) a11 = r1[ix + 1]; }
      if (!(a00 | a01 | a10 | a11)) continue;
      int t = a00 + (((a01 - a00) * fx) >> 8);
      int b = a10 + (((a11 - a10) * fx) >> 8);
      int a = t + (((b - t) * fy) >> 8);
      a = (a * alpha) >> 8;
      if (a > 0) dst[x] = blend(dst[x], color, a);
    }
  }
}

// Like drawMask, but one sample per 2x2 block of screen pixels: for the glow, which is blurry
// anyway and covers the most pixels.
void drawMaskCoarse(const Mask &m, float cx, float cy, float scale, uint16_t color, int alpha, int sy0, int sy1) {
  if (alpha <= 0) return;
  float w = m.w * scale, h = m.h * scale;
  float left = cx - w / 2, top = cy - h / 2;
  int y0 = std::max(sy0, (int)floorf(top)) & ~1, y1 = std::min(sy1, (int)ceilf(top + h));
  int bx0 = std::max(0, (int)floorf(left)) & ~1, bx1 = std::min(S, (int)ceilf(left + w));
  if (y0 >= y1 || bx0 >= bx1) return;
  const float inv = 1.0f / scale;
  const int32_t step = (int32_t)(inv * 2 * 65536.0f);
  for (int y = y0; y < y1; y += 2) {
    int32_t my = (int32_t)floorf(((y + 1.0f - top) * inv - 0.5f) * 65536.0f);
    int iy = my >> 16;
    int fy = (my >> 8) & 255;
    const uint8_t *r0 = (iy >= 0 && iy < m.h) ? m.px + iy * m.w : nullptr;
    const uint8_t *r1 = (iy + 1 >= 0 && iy + 1 < m.h) ? m.px + (iy + 1) * m.w : nullptr;
    if (!r0 && !r1) continue;
    bool row0 = y >= sy0, row1 = y + 1 < sy1;
    uint16_t *d0 = strip + (y - sy0) * S, *d1 = d0 + S;
    int32_t mx = (int32_t)floorf(((bx0 + 1.0f - left) * inv - 0.5f) * 65536.0f);
    for (int x = bx0; x < bx1; x += 2, mx += step) {
      int ix = mx >> 16;
      int fx = (mx >> 8) & 255;
      int a00 = 0, a01 = 0, a10 = 0, a11 = 0;
      bool in0 = ix >= 0 && ix < m.w, in1 = ix + 1 >= 0 && ix + 1 < m.w;
      if (r0) { if (in0) a00 = r0[ix]; if (in1) a01 = r0[ix + 1]; }
      if (r1) { if (in0) a10 = r1[ix]; if (in1) a11 = r1[ix + 1]; }
      if (!(a00 | a01 | a10 | a11)) continue;
      int t = a00 + (((a01 - a00) * fx) >> 8);
      int b = a10 + (((a11 - a10) * fx) >> 8);
      int a = ((t + (((b - t) * fy) >> 8)) * alpha) >> 8;
      if (a <= 0) continue;
      bool c1 = x + 1 < S;
      if (row0) { d0[x] = blend(d0[x], color, a); if (c1) d0[x + 1] = blend(d0[x + 1], color, a); }
      if (row1) { d1[x] = blend(d1[x], color, a); if (c1) d1[x + 1] = blend(d1[x + 1], color, a); }
    }
  }
}

void drawName(int idx, int shift, int sy0, int sy1) {
  const Item &it = items[idx];
  int left = S / 2 - it.nameW / 2;
  int top = NAME_Y - TRAY_NAME_H / 2 + shift;
  int y0 = std::max(sy0, top), y1 = std::min(sy1, top + TRAY_NAME_H);
  uint16_t white = 0xFFFF;
  for (int y = y0; y < y1; y++) {
    const uint8_t *src = it.name + (y - top) * it.nameW;
    uint16_t *dst = strip + (y - sy0) * S + left;
    for (int x = 0; x < it.nameW; x++)
      if (src[x]) dst[x] = blend(dst[x], white, src[x]);
  }
}

// Anti-aliased disc (ring if `ring` > 0: that thickness).
void drawDot(float cx, float cy, float r, float ring, uint16_t color, int alpha, int sy0, int sy1) {
  int y0 = std::max(sy0, (int)floorf(cy - r - 1)), y1 = std::min(sy1, (int)ceilf(cy + r + 1));
  int x0 = std::max(0, (int)floorf(cx - r - 1)), x1 = std::min(S, (int)ceilf(cx + r + 1));
  for (int y = y0; y < y1; y++) {
    uint16_t *dst = strip + (y - sy0) * S;
    for (int x = x0; x < x1; x++) {
      float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
      float c = fminf(1, fmaxf(0, r + 0.5f - d));
      if (ring > 0) c *= fminf(1, fmaxf(0, d - (r - ring) + 0.5f));
      int a = (int)(c * alpha);
      if (a > 0) dst[x] = blend(dst[x], color, a);
    }
  }
}

// Capsule (the handle).
void drawCapsule(float cx, float cy, float w, float h, uint16_t color, int alpha, int sy0, int sy1) {
  float rad = h / 2, half = w / 2 - rad;
  int y0 = std::max(sy0, (int)floorf(cy - rad - 1)), y1 = std::min(sy1, (int)ceilf(cy + rad + 1));
  int x0 = std::max(0, (int)floorf(cx - w / 2 - 1)), x1 = std::min(S, (int)ceilf(cx + w / 2 + 1));
  for (int y = y0; y < y1; y++) {
    uint16_t *dst = strip + (y - sy0) * S;
    for (int x = x0; x < x1; x++) {
      float px = x + 0.5f - cx, py = y + 0.5f - cy;
      float qx = fmaxf(0, fabsf(px) - half);
      float d = hypotf(qx, py);
      int a = (int)(fminf(1, fmaxf(0, rad + 0.5f - d)) * alpha);
      if (a > 0) dst[x] = blend(dst[x], color, a);
    }
  }
}

// Anti-aliased line segment with round caps, width w.
void drawSeg(float ax, float ay, float bx, float by, float w, uint16_t color, int alpha, int sy0, int sy1) {
  float r = w / 2;
  int y0 = std::max(sy0, (int)floorf(fminf(ay, by) - r - 1)), y1 = std::min(sy1, (int)ceilf(fmaxf(ay, by) + r + 1));
  int x0 = std::max(0, (int)floorf(fminf(ax, bx) - r - 1)), x1 = std::min(S, (int)ceilf(fmaxf(ax, bx) + r + 1));
  float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
  for (int y = y0; y < y1; y++) {
    uint16_t *dst = strip + (y - sy0) * S;
    for (int x = x0; x < x1; x++) {
      float px = x + 0.5f - ax, py = y + 0.5f - ay;
      float t = len2 > 0 ? fminf(1, fmaxf(0, (px * vx + py * vy) / len2)) : 0;
      float d = hypotf(px - t * vx, py - t * vy);
      int a = (int)(fminf(1, fmaxf(0, r + 0.5f - d)) * alpha);
      if (a > 0) dst[x] = blend(dst[x], color, a);
    }
  }
}

// The brightness quick button: a faint disc with a sun (display.html's QUICK icon, a 6-unit
// viewBox drawn at 64% of the button).
void drawQuick(float cy, int sy0, int sy1) {
  if (cy + QUICK_R + 2 < sy0 || cy - QUICK_R - 2 > sy1) return;
  const float cx = S / 2;
  const uint16_t white = 0xFFFF, ink = c565(INK);
  drawDot(cx, cy, QUICK_R, 0, white, 15, sy0, sy1);
  drawDot(cx, cy, QUICK_R, 1.2f, white, 31, sy0, sy1);
  const float u = QUICK_R * 2 * 0.64f / 6;  // px per viewBox unit
  const float w = 0.5f * u;
  drawDot(cx, cy, 1.1f * u + w / 2, w, ink, 191, sy0, sy1);
  for (int k = 0; k < 8; k++) {
    float a = k * (float)M_PI / 4, s = sinf(a), c = cosf(a);
    drawSeg(cx + s * 1.8f * u, cy - c * 1.8f * u, cx + s * 2.6f * u, cy - c * 2.6f * u, w, ink, 191, sy0, sy1);
  }
}

// Compose sheet rows for screen rows [sy0, sy1) of the strip; shift = screen y - sheet y.
void composeSheet(int sy0, int sy1, int shift) {
  uint32_t c0 = micros();
  // Background, generated per row; only the part of the row inside the round screen.
  for (int y = sy0; y < sy1; y++) {
    uint16_t *dst = strip + (y - sy0) * S;
    int sy = y - shift;  // sheet row
    float cy = y + 0.5f - S / 2.0f;
    int half = (int)ceilf(sqrtf(fmaxf(0, (S / 2.0f) * (S / 2.0f) - cy * cy))) + 1;
    int x0 = std::max(0, S / 2 - half), x1 = std::min(S, S / 2 + half);
    if (x0 > 0) memset(dst, 0, x0 * 2);
    if (x1 < S) memset(dst + x1, 0, (S - x1) * 2);
    int dy = sy - BG_CY;
    int32_t dy2 = dy * dy;
    // Dither by screen position, so the pattern doesn't crawl as the sheet slides.
    const uint16_t *palRow = palFast + (y & 7) * 8;
    const uint8_t *lvT = levelFast;
    const int n = bgLevelN;
    // The gradient moves less than one of its 64 steps across 8 pixels, so each aligned block
    // of 8 is one pre-dithered run from the palette (looked up at the block's middle).
    int bx0 = x0 & ~7;
    for (int x = bx0; x < x1; x += 8) {
      int dx = x + 4 - BG_CX;
      int32_t idx = (dx * dx + dy2) >> BG_SHIFT;
      int lv = idx < n ? lvT[idx] : BG_LEVELS - 1;
      const uint16_t *src = palRow + lv * 64;
      if (x >= x0 && x + 8 <= x1) memcpy(dst + x, src, 16);
      else for (int k = std::max(x, x0); k < std::min(x + 8, x1); k++) dst[k] = src[k & 7];
    }
  }
  drawQuick(QUICK_Y + shift, sy0, sy1);
  uint32_t c1 = micros();
  pcBg += c1 - c0;
  // Icons, farthest first so the chosen one sits on top.
  int order[16], n = 0;
  for (int i = 0; i < nItems; i++)
    if (fabsf(i - pos) < 3.2f) order[n++] = i;
  std::sort(order, order + n, [](int a, int b) { return fabsf(a - pos) > fabsf(b - pos); });
  const uint16_t ink = c565(INK), white = 0xFFFF, accent = c565(ACCENT);
  for (int k = 0; k < n; k++) {
    int i = order[k];
    float d = i - pos, ad = fabsf(d);
    float cx = S / 2 + itemX(d), cy = ICON_Y + shift;
    float sc = itemK(ad);
    float op = itemOp(ad);
    float close = fmaxf(0, 1 - ad * 2);  // 1 when centred, 0 half a step away
    int ga = (int)(0.5f * close * op * 255);
    uint32_t g0 = micros();
    if (ga > 2) drawMaskCoarse(items[i].glow, cx, cy, sc * 2, accent, ga, sy0, sy1);
    uint32_t g1 = micros();
    pcGlow += g1 - g0;
    int ia = (int)(op * (0.72f + 0.28f * close) * 256);
    drawMask(items[i].icon, cx, cy, sc, close > 0.5f ? white : ink, std::min(ia, 256), sy0, sy1);
    pcIcon += micros() - g1;
  }
  uint32_t c2 = micros();
  int near = nearest();
  drawName(near, shift, sy0, sy1);
  // Dots: one per panel; blue = chosen, hollow = where you came from.
  float total = nItems * DOT_R * 2 + (nItems - 1) * DOT_GAP;
  float x = S / 2 - total / 2 + DOT_R;
  for (int i = 0; i < nItems; i++, x += DOT_R * 2 + DOT_GAP) {
    float cy = DOTS_Y + shift;
    if (cy < sy0 - 10 || cy > sy1 + 10) continue;
    if (i == near) drawDot(x, cy, DOT_SEL_R, 0, accent, 255, sy0, sy1);
    else if (i == here) drawDot(x, cy, DOT_R, 1.6f, white, 140, sy0, sy1);
    else drawDot(x, cy, DOT_R, 0, white, 46, sy0, sy1);
  }
  drawCapsule(S / 2, HANDLE_Y + shift, HANDLE_W, HANDLE_H, white, 64, sy0, sy1);
  // A hairline at the sheet's bottom edge.
  int edge = S - 1 + shift;
  if (edge >= sy0 && edge < sy1) {
    uint16_t *dst = strip + (edge - sy0) * S;
    for (int xx = 0; xx < S; xx++) dst[xx] = blend(dst[xx], white, 26);
  }
  pcRest += micros() - c2;
}

// LVGL redraws screen rows [y0, y1) of whatever is under the sheet (fresh, so the shadow
// below never darkens twice). Invalidation is only open for this one area.
void lvglRows(int y0, int y1) {
  y0 = std::max(y0, 0);
  y1 = std::min(y1, S);
  if (y0 >= y1) return;
  uint32_t t0 = micros();
  lv_display_t *disp = lv_display_get_default();
  lv_display_enable_invalidation(disp, true);
  lv_area_t a = {0, y0, S - 1, y1 - 1};
  lv_obj_invalidate_area(lv_screen_active(), &a);
  lv_refr_now(disp);
  board::waitFlushIdle();
  lv_display_enable_invalidation(disp, false);
  pfLvglUs += micros() - t0;
}

// The rows below the sheet's edge: put back what was under the old shadow, have LVGL redraw
// rows the sheet uncovered, keep a clean copy of the new shadow rows, then darken them.
void underRows(int off, int prevOff, bool fresh) {
  uint16_t *fb = board::framebuffer();
  if (fresh) {
    savedN = 0;
    lvglRows(off, S);
  } else if (off < prevOff) {
    lvglRows(off, prevOff);
  }
  for (int y = std::max(off, prevOff); y < savedY0 + savedN; y++)
    if (y >= savedY0) memcpy(fb + y * S, saved + (y - savedY0) * S, S * 2);
  savedN = 0;
  if (off <= 0 || off >= S || !saved) return;
  int y1 = std::min(S, off + SHADOW);
  savedY0 = off;
  savedN = y1 - off;
  memcpy(saved, fb + off * S, (size_t)savedN * S * 2);
  for (int y = off; y < y1; y++) {
    float t = 1.0f - (float)(y - off) / SHADOW;
    int a = (int)(0.6f * t * t * 255);
    uint16_t *row = fb + y * S;
    for (int x = 0; x < S; x++) row[x] = blend(row[x], 0, a);
  }
}

void writeRows(int y0, int y1) {
  uint32_t t = micros();
  uint16_t *fb = board::framebuffer();
  memcpy(fb + y0 * S, strip, (size_t)(y1 - y0) * S * 2);
  pcWrite += micros() - t;
}

// Sheet rows [y0, y1) clipped to the visible part [0, off).
void drawSheet(int y0, int y1, int off) {
  y1 = std::min(y1, off);
  y0 = std::max(y0, 0);
  for (int y = y0; y < y1; y += STRIP) {
    int e = std::min(y + STRIP, y1);
    composeSheet(y, e, off - S);
    writeRows(y, e);
  }
}

void render() {
  uint32_t t0 = micros();
  int off = (int)lroundf(fminf(1.0f, fmaxf(0.0f, reveal)) * S);
  int near = nearest();
  bool posMoved = fabsf(pos - drawnPos) > 0.0005f;
  int lo = S, hi = 0;  // rows written, for the cache write-back
  auto touched = [&](int a, int b) { if (a < b) { lo = std::min(lo, a); hi = std::max(hi, b); } };
  if (off != drawnOff) {
    underRows(off, drawnOff, underDirty);
    touched(off, underDirty ? S : std::max(off, drawnOff) + SHADOW);
    drawSheet(0, off, off);
    touched(0, off);
  } else {
    if (posMoved || near != drawnNear) {
      drawSheet(BAND_Y0 + off - S, BAND_Y1 + off - S, off);
      touched(BAND_Y0 + off - S, std::min(off, BAND_Y1 + off - S));
    }
    if (near != drawnNear) {
      drawSheet(INFO_Y0 + off - S, INFO_Y1 + off - S, off);
      touched(INFO_Y0 + off - S, std::min(off, INFO_Y1 + off - S));
    }
    if (underDirty && off < S) { underRows(off, off, true); touched(off, S); }
  }
  bool sheetFrame = off != drawnOff;
  uint32_t us = micros() - t0;
  if (sheetFrame) { pfSheetN++; pfSheetUs += us; }
  else if (posMoved || near != drawnNear) { pfRowN++; pfRowUs += us; }
  underDirty = false;
  drawnOff = off;
  drawnPos = pos;
  drawnNear = near;
  lo = std::max(lo, 0);
  hi = std::min(hi, S);
  if (lo < hi) board::framebufferDone(lo, hi);
}

// ---------- open / close ----------

void finish();

// Lay out a screen just built under the sheet while invalidation is still off. Otherwise LVGL
// does the layout on the first lvglRows() refresh, with invalidation on, and every object it
// positions is redrawn in full over the sheet: Brightness "flashed in twice"
// (2026-11-10), worst since the dial's full-screen touch catcher invalidates the whole screen.
void settleLayout() { lv_obj_update_layout(lv_screen_active()); }

void startLive() {
  pendingOpen = false;
  if (!bgReady) return;
  strip = (uint16_t *)heap_caps_malloc((size_t)S * STRIP * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!strip) strip = (uint16_t *)heap_caps_malloc((size_t)S * STRIP * 2, MALLOC_CAP_SPIRAM);
  if (!strip) return;
  auto fast = [](size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  };
  palFast = (uint16_t *)fast(BG_LEVELS * 64 * 2);
  levelFast = (uint8_t *)fast(bgLevelN);
  saved = (uint16_t *)heap_caps_malloc((size_t)SHADOW * S * 2, MALLOC_CAP_SPIRAM);
  if (!palFast || !levelFast) {
    heap_caps_free(palFast); heap_caps_free(levelFast); heap_caps_free(saved); heap_caps_free(strip);
    palFast = nullptr; levelFast = nullptr; saved = nullptr; strip = nullptr;
    return;
  }
  memcpy(palFast, bgPal, BG_LEVELS * 64 * 2);
  memcpy(levelFast, bgLevel, bgLevelN);
  savedN = 0;
  lv_display_t *disp = lv_display_get_default();
  lv_refr_now(disp);  // anything LVGL still had to draw lands before the sheet covers it
  board::waitFlushIdle();
  lv_display_enable_invalidation(disp, false);
  list = app::shownPanels();  // hidden panels (Settings > Panels) and Weather without Wi-Fi drop out
  nItems = 0;
  here = 0;
  for (Panel *p : list) {
    for (int k = 0; k < TRAY_ASSET_COUNT; k++)
      if (!strcmp(TRAY_ASSETS[k].id, p->id)) {
        if (p == app::currentPanel()) here = nItems;
        ids[nItems] = p->id;
        nItems++;
        // items[] is filled at begin() by asset index; map it here
        break;
      }
  }
  // Re-map masks to the shown order.
  static Item byAsset[16];
  static bool mapped = false;
  if (!mapped) {
    for (int k = 0; k < TRAY_ASSET_COUNT && k < 16; k++) byAsset[k] = items[k];
    mapped = true;
  }
  for (int i = 0; i < nItems; i++)
    for (int k = 0; k < TRAY_ASSET_COUNT; k++)
      if (!strcmp(TRAY_ASSETS[k].id, ids[i])) items[i] = byAsset[k];
  pos = target = here;
  lastActivity = millis();
  reveal = 0;
  revealTo = 1;
  go = -1;
  drawnOff = 0;
  drawnPos = -99;
  drawnNear = -1;
  underDirty = false;
  isLive = true;
  lastT = millis();
  perfFrames = perfUs = perfMax = 0;
  pcBg = pcGlow = pcIcon = pcRest = pcWrite = 0;
  pfSheetN = pfSheetUs = pfRowN = pfRowUs = pfLvglUs = pfGapMax = pfLastEnd = 0;
  perfOpenAt = millis();
  Serial.printf("[tray] open, %d panels, here %s\n", nItems, ids[here]);
}

void land(int i) {
  go = -1;
  revealTo = 0;
  if (i >= 0 && i < nItems && list[i] != app::currentPanel()) {
    Serial.printf("[tray] go %s\n", list[i]->id);
    uint32_t t0 = millis();
    app::jump(list[i]);  // built now; the rows appear as the sheet slides up off them
    settleLayout();
    lastT = millis();    // a slow build (Focus's dial ~100 ms) mustn't become one big step
    underDirty = true;
    Serial.printf("[tray] built %s in %u ms\n", list[i]->id, (unsigned)(millis() - t0));
  }
}

void finish() {
  isLive = false;
  f.mode = Finger::None;
  int16_t tx, ty;
  if (board::readTouch(tx, ty)) {  // a finger still down mustn't become a tap or a new pull
    wasDown = true;
    lv_indev_wait_release(board::touchIndev());
  }
  lv_display_t *disp = lv_display_get_default();
  lv_display_enable_invalidation(disp, true);
  lv_obj_invalidate(lv_screen_active());
  heap_caps_free(strip);
  heap_caps_free(palFast);
  heap_caps_free(levelFast);
  heap_caps_free(saved);
  strip = nullptr;
  palFast = nullptr;
  levelFast = nullptr;
  saved = nullptr;
  savedN = 0;
  if (perfFrames)
    Serial.printf("[tray] closed after %u ms: %u frames, avg %.1f max %.1f ms; sheet %u x %.1f ms (lvgl %.1f total); "
                  "row %u x %.1f ms; longest gap %.1f ms\n",
                  (unsigned)(millis() - perfOpenAt), (unsigned)perfFrames, perfUs / 1000.0f / perfFrames,
                  perfMax / 1000.0f, (unsigned)pfSheetN, pfSheetN ? pfSheetUs / 1000.0f / pfSheetN : 0,
                  pfLvglUs / 1000.0f, (unsigned)pfRowN, pfRowN ? pfRowUs / 1000.0f / pfRowN : 0, pfGapMax / 1000.0f);
  if (perfFrames)
    Serial.printf("[tray] totals ms: bg %.0f glow %.0f icons %.0f rest %.0f write %.0f lvgl %.0f\n", pcBg / 1000.0f,
                  pcGlow / 1000.0f, pcIcon / 1000.0f, pcRest / 1000.0f, pcWrite / 1000.0f, pfLvglUs / 1000.0f);
}

void close() {
  go = -1;
  revealTo = 0;
}

void openBrightness() {
  close();
  Screen *t = app::topScreen();
  if (t && t->tag() && !strcmp(t->tag(), "bright")) return;  // already open underneath
  Serial.println("[tray] quick: brightness");
  app::pushNow(makeBrightnessScreen());  // ~100 ms when the dial image has to be redrawn
  settleLayout();
  lastT = millis();  // start the slide from here, or the first step jumps most of the way
  underDirty = true;  // the rows appear as the sheet slides up off them
}

void pick(int i) {
  target = std::min(std::max(i, 0), nItems - 1);
  go = (int)target;
}

// ---------- finger ----------

void sample(uint32_t t, int x) {
  if (f.ns == 12) { memmove(f.samples, f.samples + 1, sizeof(f.samples[0]) * 11); f.ns = 11; }
  f.samples[f.ns++] = {t, x};
}
float velocity(uint32_t now) {  // items per ms, from the last ~100 ms
  int i = 0;
  while (i < f.ns - 1 && now - f.samples[i].t > 100) i++;
  if (i >= f.ns - 1) return 0;
  uint32_t dt = f.samples[f.ns - 1].t - f.samples[i].t;
  if (!dt) return 0;
  return (float)(f.samples[i].x - f.samples[f.ns - 1].x) / dt / SP;
}
void hdragStart(int x) {
  f.hdrag = true;
  f.hx0 = x;
  f.hp0 = pos;
  f.moved = false;
  f.ns = 0;
}
void hdragMove(uint32_t now, int x) {
  sample(now, x);
  if (abs(x - f.hx0) > 12) f.moved = true;
  float p = f.hp0 + (f.hx0 - x) / SP;
  if (p < 0) p /= 3;
  if (p > nItems - 1) p = nItems - 1 + (p - (nItems - 1)) / 3;
  pos = p;
  target = p;
}
// Returns true if the finger actually slid.
bool hdragEnd(uint32_t now) {
  f.hdrag = false;
  float v = velocity(now);
  if (fabsf(v) < THROW_MIN) v = 0;
  target = std::min(std::max((float)lroundf(pos + v * THROW_MS + v * fabsf(v) * THROW_BOOST), 0.0f),
                    (float)(nItems - 1));
  return f.moved;
}

void fingerDown(uint32_t now, int x, int y) {
  f.mode = Finger::Open;
  f.x0 = f.lx = x;
  f.y0 = f.ly = y;
  f.downAt = now;
  f.axis = 0;
  go = -1;
}

void fingerMove(uint32_t now, int x, int y) {
  f.lx = x;
  f.ly = y;
  int dx = x - f.x0, dy = y - f.y0;
  if (f.mode == Finger::Pull) {
    reveal = fminf(1.15f, fmaxf(0.0f, dy / PULL));
    // Once the sheet is mostly down, sideways movement scrubs the row.
    if (!f.hdrag && reveal >= 0.7f) hdragStart(x);
    if (f.hdrag) hdragMove(now, x);
    return;
  }
  if (!f.axis && (abs(dx) > SLOP || abs(dy) > SLOP)) {
    f.axis = abs(dx) >= abs(dy) ? 'x' : 'y';
    if (f.axis == 'x') hdragStart(f.x0);
  }
  if (f.axis == 'x') hdragMove(now, x);
  else if (f.axis == 'y') reveal = fminf(1.0f, fmaxf(0.0f, 1.0f + dy / PULL));
}

void fingerUp(uint32_t now) {
  Finger m = f.mode;
  f.mode = Finger::None;
  if (m == Finger::Pull) {
    if (reveal < 0.4f) { f.hdrag = false; close(); return; }
    revealTo = 1;
    if (f.hdrag && hdragEnd(now)) go = (int)target;  // slid while pulling: jump on release
    return;
  }
  if (f.axis == 'x') { hdragEnd(now); return; }
  if (f.axis == 'y') { revealTo = reveal < 0.75f ? 0 : 1; if (!revealTo) go = -1; return; }
  // A tap on the brightness button: the tray slides away over the Brightness screen.
  if (hypotf(f.lx - S / 2, f.ly - QUICK_Y) < QUICK_HIT) {
    openBrightness();
    return;
  }
  // A tap: on an icon picks it.
  for (int i = 0; i < nItems; i++) {
    float d = i - pos, ad = fabsf(d);
    if (ad > 3) continue;
    float half = (TRAY_ICON / 2 + 12) * itemK(ad);
    if (fabsf(f.lx - (S / 2 + itemX(d))) < half && fabsf(f.ly - ICON_Y) < TRAY_ICON / 2 + 12) {
      pick(i);
      return;
    }
  }
}

bool readFinger(uint32_t now, int &x, int &y) {
  if (script.on) {
    // Pull 230 px down over 180 ms, hold 60 ms, then flick sideways over 90 ms and let go.
    uint32_t t = now - script.t0;
    int travel = script.items * (int)SP / 2;
    if (t < 180) { x = 240; y = 30 + (int)(230 * t / 180.0f); return true; }
    if (t < 240) { x = 240; y = 260; return true; }
    if (t < 330) { x = 240 - travel * (int)(t - 240) / 90; y = 262; return true; }
    script.on = false;
    return false;
  }
  int16_t tx, ty;
  if (!board::readTouch(tx, ty)) return false;
  x = tx;
  y = ty;
  return true;
}


void pollFinger(uint32_t now) {
  int x, y;
  bool down = readFinger(now, x, y);
  if (down || f.mode != Finger::None) lastActivity = now;
  if (down) {
    if (f.mode == Finger::None) {
      if (reveal < 0.05f && revealTo == 0) return;  // closing: ignore
      fingerDown(now, x, y);
    } else {
      fingerMove(now, x, y);
    }
    if (f.mode == Finger::Open && !f.axis && now - f.downAt >= HOLD_MS) {
      f.mode = Finger::None;  // a still finger held: close
      f.axis = 'h';
      close();
    }
  } else if (f.mode != Finger::None) {
    fingerUp(now);
  }
}

// ---------- background ----------

void makeBgTables() {
  const dither::RGB c0 = dither::rgb(0x1A2030), c1 = dither::rgb(0x0D1016);
  const float r = 0.72f * hypotf(240, 336);  // 72% of the distance to the farthest corner
  bgPal = (uint16_t *)heap_caps_malloc(BG_LEVELS * 64 * 2, MALLOC_CAP_SPIRAM);
  int maxD2 = 240 * 240 + 336 * 336;
  bgLevelN = (maxD2 >> BG_SHIFT) + 1;
  bgLevel = (uint8_t *)heap_caps_malloc(bgLevelN, MALLOC_CAP_SPIRAM);
  if (!bgPal || !bgLevel) return;
  for (int lv = 0; lv < BG_LEVELS; lv++) {
    dither::RGB c = dither::mix(c0, c1, (float)lv / (BG_LEVELS - 1));
    for (int by = 0; by < 8; by++)
      for (int bx = 0; bx < 8; bx++) bgPal[by * 8 + lv * 64 + bx] = dither::to565(c, bx, by);
  }
  for (int i = 0; i < bgLevelN; i++) {
    float d = sqrtf((float)((i << BG_SHIFT) + (1 << (BG_SHIFT - 1))));
    bgLevel[i] = (uint8_t)std::min(BG_LEVELS - 1, (int)lroundf(fminf(1.0f, d / r) * (BG_LEVELS - 1)));
  }
  bgReady = true;
}

}  // namespace

void begin() {
  for (int k = 0; k < TRAY_ASSET_COUNT && k < 16; k++) {
    const TrayAsset &a = TRAY_ASSETS[k];
    Item &it = items[k];
    it.icon.px = a.icon;
    it.icon.w = it.icon.h = TRAY_ICON;
    makeSpans(it.icon);
    it.glow.px = a.glow;
    it.glow.w = it.glow.h = TRAY_GLOW_HALF;
    makeSpans(it.glow);
    it.name = a.name;
    it.nameW = a.nameW;
  }
  makeBgTables();
  board::setTouchHook(touchSample);
}

bool live() { return isLive || pendingOpen; }

bool touchSample(bool down, int x, int y) {
  if (isLive || pendingOpen) return true;
  if (down && !wasDown) {
    cand = y < EDGE && app::fastTravelAllowed() && bgReady;
    candX = x;
    candY = y;
  }
  wasDown = down;
  if (!down) { cand = false; return false; }
  if (!cand) return false;
  int dx = x - candX, dy = y - candY;
  if (abs(dx) <= SLOP && abs(dy) <= SLOP) return false;
  cand = false;
  if (dy > 0 && dy >= abs(dx) * 0.8f) {
    // A pull: LVGL forgets this touch (no click on the nav pill it started on).
    lv_indev_reset(board::touchIndev(), nullptr);
    pendingOpen = true;
    f.mode = Finger::Pull;
    f.x0 = candX;
    f.y0 = candY;
    f.lx = x;
    f.ly = y;
    f.downAt = millis();
    f.hdrag = false;
    f.axis = 0;
    return true;
  }
  return false;
}

void loop() {
  if (pendingOpen) {
    startLive();
    if (!isLive) { f.mode = Finger::None; return; }
  }
  if (!isLive) return;
  uint32_t t0 = micros();
  if (pfLastEnd) pfGapMax = std::max(pfGapMax, t0 - pfLastEnd);
  uint32_t now = millis();
  float dt = (float)std::min<uint32_t>(64, std::max<uint32_t>(1, now - lastT));
  lastT = now;

  if (app::alertWaiting()) close();  // an alert must be seen
  pollFinger(now);

  bool fingerOnRow = f.hdrag;
  bool fingerOnSheet = f.mode == Finger::Pull || (f.mode == Finger::Open && f.axis == 'y');
  if (!fingerOnRow) {
    float dp = target - pos;
    if (fabsf(dp) < 0.003f) pos = target;
    else pos += dp * (1 - expf(-dt / TAU_POS));
  }
  if (!fingerOnSheet) {
    float dr = revealTo - reveal;
    if (fabsf(dr) < 0.004f) reveal = revealTo;
    else reveal += dr * (1 - expf(-dt / TAU_SHEET));
  }
  // Resting open on a panel with nothing touched for 5 s: go there (on the one it opened
  // from, that just closes it).
  if (go < 0 && f.mode == Finger::None && revealTo == 1 && reveal > 0.99f && pos == target &&
      now - lastActivity >= SETTLE_MS) {
    Serial.println("[tray] settled");
    pick(nearest());
  }
  if (go >= 0 && fabsf(target - pos) < 0.02f && reveal > 0.9f) {
    pos = target;
    render();
    land(go);
  }

  render();
  uint32_t us = micros() - t0;
  perfFrames++;
  perfUs += us;
  perfMax = std::max(perfMax, us);
  pfLastEnd = micros();

  if (reveal <= 0.004f && revealTo == 0 && f.mode == Finger::None) {
    reveal = 0;
    finish();
  }
}

void rotate(int dir) {
  if (!isLive || go >= 0) return;
  lastActivity = millis();
  // Knob acceleration like Pong: clicks closer than 150 ms move up to 3 panels at a time.
  static uint32_t last = 0;
  static int lastDir = 0;
  uint32_t now = millis(), g = now - last;
  int mult = 1;
  if (dir == lastDir && g < 150) mult = (int)constrain(1 + (150 - (int)g) / 50.0f, 1.0f, 3.0f);
  last = now;
  lastDir = dir;
  target = std::min(std::max((float)lroundf(target) + dir * mult, 0.0f), (float)(nItems - 1));
  if (revealTo == 0) revealTo = 1;
}

void press() {
  if (!isLive || go >= 0) return;
  lastActivity = millis();
  if (revealTo == 1 && reveal > 0.9f) pick((int)lroundf(target));
}

void hold() {
  if (isLive) close();
}

void home() {
  if (!isLive) return;
  for (int i = 0; i < nItems; i++)
    if (!strcmp(ids[i], "home")) { pick(i); return; }
}

void swipe(int dir) { rotate(dir); }

void testToggle() {
  if (isLive) { close(); return; }
  pendingOpen = true;
  f.mode = Finger::None;
}

void testQuick() {
  if (isLive && revealTo == 1) openBrightness();
}

void testFlick(int items) {
  if (isLive || pendingOpen) return;
  script.on = true;
  script.t0 = millis();
  script.items = items;
  pendingOpen = true;
  f.mode = Finger::Pull;
  f.x0 = 240;
  f.y0 = 30;
  f.lx = 240;
  f.ly = 30;
  f.downAt = millis();
  f.hdrag = false;
  f.axis = 0;
}

}  // namespace tray
