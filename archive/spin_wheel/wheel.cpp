// Spin the wheel: the pick F2 from wheel-mockups.html (wheelMode in display.html). A neon
// band of segments rides the screen's edge; the middle shows the option under the pointer in
// its color as it flies past (label, thin ring, soft halo), then settles and the halo blooms.
// Rim labels follow the curve and turn upright wherever the wheel stops.
//
// Rendering:
//  - The band is pre-drawn per wheel set into polar textures (columns = angle, rows = radius).
//    Labels are drawn into them as straight text, which comes out curved on screen. There are
//    two textures, labels reading clockwise (upright at the top) and counter-clockwise
//    (upright at the bottom); at rest each segment uses whichever is upright where it stopped.
//  - At rest the band is drawn into LVGL's full-screen image (sharp, dithered).
//  - While spinning, LVGL would have to copy the whole screen twice per frame (8 fps), so the
//    band is written straight into the panel's framebuffer by a task on the other core, from
//    a compact copy of the texture (8-bit palette, 0.5 degree columns) held in internal RAM;
//    external RAM was too slow for its random reads. LVGL only redraws the middle.
//  - The middle (halo, thin ring, pointer) comes from precomputed alpha masks and is redrawn
//    only when its color changes.
// The lookup tables take ~2 s to compute, so wheelPrepare() builds them at boot on core 0.
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>

#include "board.h"
#include "dither.h"
#include "modes.h"
#include "store.h"
#include "theme.h"

namespace {

constexpr float U = 4.8f;  // display.html viewBox unit in px
constexpr int A = 1440;    // texture columns: 0.25 degree each
constexpr int R0 = 150;    // texture row 0 = this radius (px)
constexpr int ROWS = 90;   // up to the screen's edge
constexpr float BAND_IN = 33 * U, BAND_OUT = 49.5f * U;
constexpr float RIM_IN = 46 * U, RIM_OUT = 49 * U;
constexpr float DIRECT_R = 146;  // while spinning, pixels from here out bypass LVGL
constexpr int CEN0 = 82, CEN = 316;  // the middle's square: 82..398
constexpr uint32_t SCREEN_BG = 0x0B0D11;
// The spinning copy: 720 columns (0.5 degree) x 64 rows from the band's inner edge out.
constexpr int SA = 720, SROWS = 64;
constexpr float SR0 = 158, SRSTEP = (240 - SR0) / SROWS;

struct WheelSet {
  const char *name;
  std::vector<const char *> opts;
};
const WheelSet SETS[] = {
  {"Yes or no", {"Yes", "No"}},
  {"Yes, no, maybe", {"Yes", "No", "Maybe"}},
  {"Lunch", {"Tacos", "Pizza", "Salad", "Sushi", "Burgers", "Thai"}},
  {"Next step", {"Ship it", "Iterate", "Ask someone", "Sleep on it"}},
  {"Pick a number", {"1", "2", "3", "4", "5", "6", "7", "8"}},
};
constexpr int NSETS = sizeof(SETS) / sizeof(SETS[0]);
const uint32_t COLORS[] = {0x4F8CFF, 0xA66BFF, 0xFF5FD2, 0xFF6B6B, 0xF5B942, 0x3FCF7F, 0x2BD4D4, 0xFF9A3C};

int setIdx() { return constrain(store::getInt("wheelset", 2), 0, NSETS - 1); }

// ---------- buffers (PSRAM unless noted, kept between visits) ----------
uint16_t *screenBuf = nullptr;            // LVGL's image of the screen: 480x480 RGB565
uint16_t *tex[2] = {nullptr, nullptr};    // ROWS x A: labels clockwise / counter-clockwise
int texSet = -1;                           // which set tex[] holds (kept between visits)
uint8_t segOfCol[A];                      // which segment each texture column belongs to
bool flipped[8];                          // segment i uses tex[1]
// At rest, every ring pixel (r >= BAND_IN): screen index | texture row << 18, and its column.
uint32_t *ringPx = nullptr;
uint16_t *ringAng = nullptr;
int ringCount = 0;
// While spinning: the band as runs along screen rows, 2 bytes per pixel (row << 10 | column),
// and the thin-ring zone between DIRECT_R and the band, copied from screenBuf when it changes.
struct Run { uint16_t y, x0, len; uint32_t code; };  // code: index of its first entry
Run *runs = nullptr;
int runCount = 0;
uint16_t *codes = nullptr;
uint32_t *zone = nullptr;
int zoneCount = 0;
uint8_t *haloA = nullptr, *rimA = nullptr, *ptrA = nullptr;  // CEN x CEN masks
lv_image_dsc_t screenImg;
volatile bool tablesReady = false;
bool tablesStarted = false;

void *ps(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
float clamp01(float x) { return fmaxf(0, fminf(1, x)); }

// A thin glowing line's alpha at distance d from its center line (half width hw): the solid
// core plus the faux display's blur(0.9 unit) glow.
float glowLine(float d, float hw) {
  float core = clamp01(hw + 0.5f - d);
  float o = fmaxf(0, d - hw);
  return fmaxf(core, 0.55f * expf(-(o * o) / (2 * 4.3f * 4.3f)));
}

inline uint16_t pack(int r, int g, int b, int x, int y) {
  int t = dither::BAYER8[y & 7][x & 7] * 4 + 2;  // 0..255
  int r5 = (r * 31 + t) / 255, g6 = (g * 63 + t) / 255, b5 = (b * 31 + t) / 255;
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}
inline void unpack(uint16_t c, int &r, int &g, int &b) {
  r = (c >> 11) * 255 / 31;
  g = ((c >> 5) & 63) * 255 / 63;
  b = (c & 31) * 255 / 31;
}

std::atomic<bool> texAllocated{false};
void allocTextures() {  // called from both the UI (boot) and the prep task: allocate once
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  static bool claimed = false;
  bool mine = false;
  portENTER_CRITICAL(&mux);
  if (!claimed) claimed = mine = true;
  portEXIT_CRITICAL(&mux);
  if (mine) {
    tex[0] = (uint16_t *)ps(ROWS * A * 2);
    tex[1] = (uint16_t *)ps(ROWS * A * 2);
    texAllocated = true;
  }
  while (!texAllocated) delay(1);
}

void buildTables() {
  uint32_t t0 = millis();
  screenBuf = (uint16_t *)ps(480 * 480 * 2);
  allocTextures();
  haloA = (uint8_t *)ps(CEN * CEN);
  rimA = (uint8_t *)ps(CEN * CEN);
  ptrA = (uint8_t *)ps(CEN * CEN);
  int nBand = 0, nZone = 0;
  for (int y = 0; y < 480; y++) {
    if ((y & 15) == 15) vTaskDelay(1);  // let core 0 idle: its task watchdog panics after 5 s
    for (int x = 0; x < 480; x++) {
      float d = hypotf(x + 0.5f - 240, y + 0.5f - 240);
      if (d >= SR0 && d < 240) nBand++;
      else if (d >= DIRECT_R && d < SR0) nZone++;
    }
  }
  ringPx = (uint32_t *)ps(nBand * 4);
  ringAng = (uint16_t *)ps(nBand * 2);
  codes = (uint16_t *)ps(nBand * 2);
  zone = (uint32_t *)ps(nZone * 4);
  runs = (Run *)ps(480 * 2 * sizeof(Run));
  ringCount = zoneCount = runCount = 0;
  for (int y = 0; y < 480; y++) {
    if ((y & 7) == 7) vTaskDelay(1);
    bool inRun = false;
    for (int x = 0; x < 480; x++) {
      float dx = x + 0.5f - 240, dy = y + 0.5f - 240;
      float d = hypotf(dx, dy);
      bool band = d >= SR0 && d < 240;
      if (!band) {
        inRun = false;
        if (d >= DIRECT_R && d < SR0) zone[zoneCount++] = y * 480 + x;
        continue;
      }
      if (!inRun) {
        runs[runCount++] = {(uint16_t)y, (uint16_t)x, 0, (uint32_t)ringCount};
        inRun = true;
      }
      runs[runCount - 1].len++;
      float th = atan2f(dx, -dy);  // clockwise from the top
      if (th < 0) th += 2 * M_PI;
      int col = (int)(th / (2 * M_PI) * A) % A;
      int row = constrain((int)(d - R0), 0, ROWS - 1);
      ringPx[ringCount] = (uint32_t)(y * 480 + x) | ((uint32_t)row << 18);
      ringAng[ringCount] = col;
      int srow = constrain((int)((d - SR0) / SRSTEP), 0, SROWS - 1);
      codes[ringCount] = (uint16_t)(srow << 10 | (col / (A / SA)));
      ringCount++;
    }
  }
  // Middle masks: halo (r 24, blur 2.2 units), ring (r 32.4, stroke 0.6), pointer triangle.
  const float tip[2] = {240, 32.6f * U}, bl[2] = {47.4f * U, 37 * U}, br[2] = {52.6f * U, 37 * U};
  auto edge = [](const float *a, const float *b, float x, float y) {  // signed distance, + inside
    float ex = b[0] - a[0], ey = b[1] - a[1], len = hypotf(ex, ey);
    return ((y - a[1]) * ex - (x - a[0]) * ey) / len;
  };
  for (int y = 0; y < CEN; y++) {
    if ((y & 7) == 7) vTaskDelay(1);
    for (int x = 0; x < CEN; x++) {
      float px = CEN0 + x + 0.5f, py = CEN0 + y + 0.5f;
      float d = hypotf(px - 240, py - 240);
      float s = 2.2f * U;
      haloA[y * CEN + x] = (uint8_t)(0.5f * erfcf((d - 24 * U) / (s * 1.41421f)) * 255);
      rimA[y * CEN + x] = (uint8_t)(glowLine(fabsf(d - 32.4f * U), 0.3f * U) * 255);
      float e = fminf(fminf(edge(tip, br, px, py), edge(br, bl, px, py)), edge(bl, tip, px, py));
      float a = e >= 0 ? clamp01(e + 0.5f) : fmaxf(clamp01(e + 0.5f), 0.55f * expf(-(e * e) / (2 * 4.3f * 4.3f)));
      ptrA[y * CEN + x] = (uint8_t)(a * 255);
    }
  }
  uint16_t bg = pack(0x0B, 0x0D, 0x11, 0, 0);
  for (int i = 0; i < 480 * 480; i++) screenBuf[i] = bg;
  memset(&screenImg, 0, sizeof(screenImg));
  screenImg.header.magic = LV_IMAGE_HEADER_MAGIC;
  screenImg.header.cf = LV_COLOR_FORMAT_RGB565;
  screenImg.header.w = 480;
  screenImg.header.h = 480;
  screenImg.header.stride = 480 * 2;
  screenImg.data_size = 480 * 480 * 2;
  screenImg.data = (const uint8_t *)screenBuf;
  Serial.printf("[wheel] tables in %lu ms: %d band pixels in %d runs, %d ring-zone pixels, PSRAM free %u KB\n",
                (unsigned long)(millis() - t0), ringCount, runCount, zoneCount,
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  tablesReady = true;
}

void waitForTables() {
  if (!tablesStarted) {
    tablesStarted = true;
    buildTables();
  }
  while (!tablesReady) delay(10);
}

// ---------- the band textures ----------

// Label text: drawn by LVGL into a small ARGB8888 canvas, then kept as alpha (internal RAM)
// plus a blurred copy for the glow.
constexpr int MASK_W = 360, MASK_H = 40;
uint32_t *canvasBuf = nullptr;
uint8_t mask[MASK_H][MASK_W], glowMask[MASK_H][MASK_W];

struct Mask {
  int w, h;
  float mid;  // the caps' vertical middle, in mask rows
};

Mask renderText(lv_obj_t *canvas, const char *text, const lv_font_t *font, int width) {
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
  lv_layer_t layer;
  lv_canvas_init_layer(canvas, &layer);
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = text;
  d.font = font;
  d.color = lv_color_white();
  d.letter_space = 2;
  lv_area_t a = {0, 0, MASK_W - 1, MASK_H - 1};
  lv_draw_label(&layer, &d, &a);
  lv_canvas_finish_layer(canvas, &layer);
  int lh = min((int)lv_font_get_line_height(font), MASK_H);
  int w = min(width, MASK_W);
  for (int y = 0; y < MASK_H; y++)
    for (int x = 0; x < MASK_W; x++) mask[y][x] = canvasBuf[y * MASK_W + x] >> 24;
  // Glow: a 7x7 box blur (two passes of a 1D box), roughly the faux display's blur(0.9).
  static uint16_t tmp[MASK_H][MASK_W];
  for (int y = 0; y < MASK_H; y++)
    for (int x = 0; x < MASK_W; x++) {
      int s = 0;
      for (int k = -3; k <= 3; k++) s += (x + k >= 0 && x + k < MASK_W) ? mask[y][x + k] : 0;
      tmp[y][x] = s;
    }
  for (int y = 0; y < MASK_H; y++)
    for (int x = 0; x < MASK_W; x++) {
      int s = 0;
      for (int k = -3; k <= 3; k++) s += (y + k >= 0 && y + k < MASK_H) ? tmp[y + k][x] : 0;
      glowMask[y][x] = min(255, s * 2 / 49);
    }
  float base = lh - font->base_line;  // baseline row
  return {w, lh, base - lh * 0.29f};  // caps are ~0.58 of the line height
}

inline float sample(const uint8_t m[MASK_H][MASK_W], float x, float y) {
  if (x < 0 || y < 0 || x >= MASK_W - 1 || y >= MASK_H - 1) return 0;
  int x0 = (int)x, y0 = (int)y;
  float fx = x - x0, fy = y - y0;
  float top = m[y0][x0] * (1 - fx) + m[y0][x0 + 1] * fx;
  float bot = m[y0 + 1][x0] * (1 - fx) + m[y0 + 1][x0 + 1] * fx;
  return (top * (1 - fy) + bot * fy) / 255.0f;
}

// Paint the band for `set` into both textures.
void buildTextures(int set, lv_obj_t *canvas) {
  uint32_t t0 = millis();
  const WheelSet &S = SETS[set];
  const int n = S.opts.size();
  const float SEG = 360.0f / n;
  const float bgR = 0x0B, bgG = 0x0D, bgB = 0x11;
  // Base: tint, glowing rim, hairlines between segments.
  for (int col = 0; col < A; col++) segOfCol[col] = min(n - 1, (int)((col + 0.5f) * 360.0f / A / SEG));
  for (int row = 0; row < ROWS; row++) {
    float r = R0 + row + 0.5f;
    float inBand = clamp01(r - BAND_IN + 0.5f) * clamp01(BAND_OUT + 0.5f - r);
    float rim = glowLine(fabsf(r - (RIM_IN + RIM_OUT) / 2), (RIM_OUT - RIM_IN) / 2) * clamp01(r - BAND_IN + 4);
    float tint = 0.10f * inBand;
    float lineA = 0.16f * inBand;
    for (int col = 0; col < A; col++) {
      int i = segOfCol[col];
      uint32_t c = COLORS[i % 8];
      float cr = c >> 16, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
      float pr = bgR + (cr - bgR) * tint, pg = bgG + (cg - bgG) * tint, pb = bgB + (cb - bgB) * tint;
      pr += (cr - pr) * rim; pg += (cg - pg) * rim; pb += (cb - pb) * rim;
      float deg = (col + 0.5f) * 360.0f / A;
      float edgeDeg = fminf(deg - i * SEG, (i + 1) * SEG - deg);
      float la = lineA * clamp01(1.35f - edgeDeg * (float)M_PI / 180 * r);
      pr += (255 - pr) * la; pg += (255 - pg) * la; pb += (255 - pb) * la;
      uint16_t v = pack((int)pr, (int)pg, (int)pb, col, row);
      tex[0][row * A + col] = v;
      tex[1][row * A + col] = v;
    }
  }
  // Labels along the curve: tex[0] reads clockwise (upright at the top), tex[1] the other way.
  if (!canvasBuf) canvasBuf = (uint32_t *)ps(MASK_W * MASK_H * 4);
  lv_canvas_set_buffer(canvas, canvasBuf, MASK_W, MASK_H, LV_COLOR_FORMAT_ARGB8888);
  const float rc = 39.9f * U;  // the labels' middle radius
  const lv_font_t *FONTS[] = {&lv_font_montserrat_20, &lv_font_montserrat_18, &lv_font_montserrat_16,
                              &lv_font_montserrat_14, &lv_font_montserrat_12};
  for (int i = 0; i < n; i++) {
    String up = S.opts[i];
    up.toUpperCase();
    float pad = fminf(4, SEG * 0.12f);
    float room = (SEG - 2 * pad) * M_PI / 180 * rc;
    const lv_font_t *font = FONTS[4];
    lv_point_t sz = {0, 0};
    for (const lv_font_t *f : FONTS) {  // shrink long labels to fit
      lv_text_get_size(&sz, up.c_str(), f, 2, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      font = f;
      if (sz.x <= room) break;
    }
    Mask m = renderText(canvas, up.c_str(), font, sz.x);
    float midDeg = (i + 0.5f) * SEG;
    int cr = COLORS[i % 8] >> 16, cg = (COLORS[i % 8] >> 8) & 0xFF, cb = COLORS[i % 8] & 0xFF;
    float halfDeg = (m.w / 2.0f + 6) / rc * 180 / M_PI;
    int c0 = (int)floorf((midDeg - halfDeg) / 360 * A), c1 = (int)ceilf((midDeg + halfDeg) / 360 * A);
    int r0 = max(0, (int)(rc - R0 - m.h / 2 - 6)), r1 = min(ROWS - 1, (int)(rc - R0 + m.h / 2 + 6));
    for (int t = 0; t < 2; t++) {
      bool ccw = t == 1;
      for (int row = r0; row <= r1; row++) {
        float r = R0 + row + 0.5f;
        for (int cc = c0; cc <= c1; cc++) {
          int col = (cc % A + A) % A;
          float s = ((cc + 0.5f) * 360.0f / A - midDeg) * M_PI / 180 * rc;  // along the arc
          float bx = ccw ? m.w / 2.0f - s : m.w / 2.0f + s;
          float by = ccw ? m.mid + (r - rc) : m.mid - (r - rc);
          float a = sample(mask, bx - 0.5f, by - 0.5f);
          float g = sample(glowMask, bx - 0.5f, by - 0.5f) * 0.5f;
          if (a <= 0.003f && g <= 0.003f) continue;
          uint16_t &px = tex[t][row * A + col];
          int pr, pg, pb;
          unpack(px, pr, pg, pb);
          float k = 1 - (1 - a) * (1 - g);  // glow, then the text over it
          px = pack(pr + (int)((cr - pr) * k), pg + (int)((cg - pg) * k), pb + (int)((cb - pb) * k), col, row);
        }
      }
    }
  }
  texSet = set;
  Serial.printf("[wheel] textures for %s in %lu ms\n", S.name, (unsigned long)(millis() - t0));
}

// Which way each segment's label reads, for the wheel at rest at `rot` degrees.
void layLabels(int set, float rot) {
  int n = SETS[set].opts.size();
  for (int i = 0; i < n; i++) {
    float shown = fmodf((i + 0.5f) * 360.0f / n + rot, 360);
    if (shown < 0) shown += 360;
    flipped[i] = shown > 90 && shown < 270;
  }
}

// The band at rest, at `rot`, into LVGL's image.
void drawBandAtRest(float rot) {
  int off = (int)lroundf(rot / 360.0f * A) % A;
  if (off < 0) off += A;
  for (int i = 0; i < ringCount; i++) {
    uint32_t p = ringPx[i];
    int col = ringAng[i] - off;
    if (col < 0) col += A;
    screenBuf[p & 0x3FFFF] = tex[flipped[segOfCol[col]] ? 1 : 0][(p >> 18) * A + col];
  }
}

// ---------- spinning ----------

// The compact texture (internal RAM, only while spinning) and its palette: 32 shades per
// segment, from the screen color to the segment's color, plus two for the white hairlines.
uint8_t *spinTex = nullptr;  // SROWS x SA
uint16_t pal[256];

void buildSpinTex(int set) {
  uint32_t t0 = millis();
  spinTex = (uint8_t *)heap_caps_malloc(SROWS * SA, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!spinTex) spinTex = (uint8_t *)ps(SROWS * SA);  // slower, still works
  const int n = SETS[set].opts.size();
  struct C { int r, g, b; };
  static C palRgb[256];
  const C bg = {0x0B, 0x0D, 0x11};
  for (int s = 0; s < n; s++) {
    uint32_t c = COLORS[s % 8];
    C col = {(int)(c >> 16), (int)((c >> 8) & 0xFF), (int)(c & 0xFF)};
    for (int k = 0; k < 32; k++) {
      C v;
      if (k < 30) {
        float a = powf(k / 29.0f, 1.6f);  // more shades in the dark tint
        v = {bg.r + (int)((col.r - bg.r) * a), bg.g + (int)((col.g - bg.g) * a), bg.b + (int)((col.b - bg.b) * a)};
      } else {
        float w = k == 30 ? 0.16f : 0.35f;  // hairline over the tint
        C t = {bg.r + (int)((col.r - bg.r) * 0.1f), bg.g + (int)((col.g - bg.g) * 0.1f), bg.b + (int)((col.b - bg.b) * 0.1f)};
        v = {t.r + (int)((255 - t.r) * w), t.g + (int)((255 - t.g) * w), t.b + (int)((255 - t.b) * w)};
      }
      palRgb[s * 32 + k] = v;
      pal[s * 32 + k] = (uint16_t)(((v.r * 31 + 127) / 255) << 11 | ((v.g * 63 + 127) / 255) << 5 | ((v.b * 31 + 127) / 255));
    }
  }
  for (int r6 = 0; r6 < SROWS; r6++) {
    int row = constrain((int)(SR0 + (r6 + 0.5f) * SRSTEP - R0), 0, ROWS - 1);
    for (int c = 0; c < SA; c++) {
      int col = c * (A / SA);
      int s = segOfCol[col];
      int r, g, b;
      unpack(tex[flipped[s] ? 1 : 0][row * A + col], r, g, b);
      int best = 0, bestD = INT32_MAX;
      for (int k = 0; k < 32; k++) {
        const C &p = palRgb[s * 32 + k];
        int d = (p.r - r) * (p.r - r) + (p.g - g) * (p.g - g) + (p.b - b) * (p.b - b);
        if (d < bestD) { bestD = d; best = k; }
      }
      spinTex[r6 * SA + c] = s * 32 + best;
    }
  }
  Serial.printf("[wheel] spin texture in %lu ms\n", (unsigned long)(millis() - t0));
}

// The band writer's helper on core 0: draws its share of each frame's rows while core 1
// draws the rest.
volatile int bandFrames = 0;
volatile uint32_t bandUs = 0;

// Runs [from, to) of the band at `rot`, straight into the framebuffer.
void drawSpinRuns(float rot, int from, int to) {
  uint16_t *fb = board::framebuffer();
  int off = (int)lroundf(rot / 360.0f * SA) % SA;
  if (off < 0) off += SA;
  for (int k = from; k < to; k++) {
    const Run &r = runs[k];
    const uint16_t *code = codes + r.code;
    uint16_t *dst = fb + r.y * 480 + r.x0;
    for (int i = 0; i < r.len; i++) {
      uint16_t c = code[i];
      int col = (c & 1023) - off;
      if (col < 0) col += SA;
      dst[i] = pal[spinTex[(c >> 10) * SA + col]];
    }
  }
}

// One spin frame, drawn on core 1, then the cache is written back. Splitting rows with a
// core-0 task measured slower (15 fps vs 17), since core 0 also runs the panel interrupt.
void drawSpinFrame(float rot) {
  drawSpinRuns(rot, 0, runCount);
  board::framebufferDone();
}


// Copy the thin-ring zone (between LVGL's part and the band) from screenBuf to the screen.
void copyZone() {
  uint16_t *fb = board::framebuffer();
  for (int i = 0; i < zoneCount; i++) fb[zone[i]] = screenBuf[zone[i]];
}

// ---------- the middle ----------

// The halo in `halo` at opacity `op`, the thin ring in `ring`, the white pointer.
void drawMiddle(uint32_t halo, float op, uint32_t ring) {
  const int bgR = SCREEN_BG >> 16, bgG = (SCREEN_BG >> 8) & 0xFF, bgB = SCREEN_BG & 0xFF;
  const int hR = halo >> 16, hG = (halo >> 8) & 0xFF, hB = halo & 0xFF;
  const int rR = ring >> 16, rG = (ring >> 8) & 0xFF, rB = ring & 0xFF;
  const int hop = (int)(op * 256);
  const float lim = (SR0 - 0.5f) * (SR0 - 0.5f);
  for (int y = 0; y < CEN; y++) {
    int sy = CEN0 + y;
    float dy = sy + 0.5f - 240;
    for (int x = 0; x < CEN; x++) {
      int sx = CEN0 + x;
      float dx = sx + 0.5f - 240;
      if (dx * dx + dy * dy >= lim) continue;  // the band's
      int k = y * CEN + x;
      int r = bgR, g = bgG, b = bgB;
      int a = haloA[k] * hop >> 8;
      r += (hR - r) * a >> 8; g += (hG - g) * a >> 8; b += (hB - b) * a >> 8;
      a = rimA[k];
      r += (rR - r) * a >> 8; g += (rG - g) * a >> 8; b += (rB - b) * a >> 8;
      a = ptrA[k];
      r += (255 - r) * a >> 8; g += (255 - g) * a >> 8; b += (255 - b) * a >> 8;
      screenBuf[sy * 480 + sx] = pack(r, g, b, sx, sy);
    }
  }
}

// Ask LVGL to redraw only inside DIRECT_R: horizontal strips, each as wide as the circle at
// the strip's edge farthest from the center.
void invalidateMiddle(lv_obj_t *img) {
  const int R = (int)DIRECT_R - 1, STEP = 20;
  for (int y0 = 240 - R; y0 < 240 + R; y0 += STEP) {
    int y1 = min(y0 + STEP - 1, 240 + R - 1);
    int dy = max(abs(y0 - 240), abs(y1 + 1 - 240));
    int half = (int)sqrtf(fmaxf(0, (float)(R * R - dy * dy)));
    if (half <= 0) continue;
    lv_area_t a = {240 - half, y0, 240 + half - 1, y1};
    lv_obj_invalidate_area(img, &a);
  }
}

// ---------- the screen ----------
struct WheelMode : Screen {
  lv_obj_t *img = nullptr, *label = nullptr, *sub = nullptr, *canvas = nullptr;
  float rot = 0;
  int builtSet = -1;
  bool spinning = false, spun = false;
  float from = 0, to = 0;
  uint32_t spinStart = 0, bloomStart = 0;
  int shown = -2;  // segment shown in the middle (-1 = the "Spin" invitation)
  int tinted = -2;  // segment whose color the halo and ring have
  uint32_t tintedAt = 0;
  static constexpr uint32_t SPIN_MS = 4200, BLOOM_MS = 600;

  ~WheelMode() override { endSpin(); }

  int under() const {
    int n = SETS[builtSet].opts.size();
    float a = fmodf(360 - fmodf(rot, 360), 360);
    if (a < 0) a += 360;
    return min(n - 1, (int)(a / (360.0f / n)));
  }

  void build(lv_obj_t *root) override {
    waitForTables();
    if (spinning) {  // rebuilt mid-spin (an alert came and went): finish it at once
      endSpin();
      rot = fmodf(to, 360);
    }
    img = lv_image_create(root);
    lv_image_set_src(img, &screenImg);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, [](lv_event_t *e) { ((WheelMode *)lv_event_get_user_data(e))->press(); },
                        LV_EVENT_SHORT_CLICKED, this);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);  // hold reaches the screen (back)
    canvas = lv_canvas_create(root);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    label = ui::label(root, "", &lv_font_montserrat_48, theme::ink());
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    sub = ui::label(root, "", &lv_font_montserrat_16, theme::inkDim());
    lv_obj_set_style_text_letter_space(sub, 3, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, (int32_t)(57.5f * U));
    lv_obj_t *back = ui::chip(root, "Back", ui::Tone::Normal, [](lv_event_t *) { app::pop(); }, nullptr);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, (int32_t)(69 * U));
    shown = tinted = -2;
    showSet();
  }

  void showSet() {
    int s = setIdx();
    if (s != builtSet) {
      builtSet = s;
      rot = 0;
      spun = false;
      if (s != texSet) buildTextures(s, canvas);  // kept from the last visit (or boot) otherwise
    }
    layLabels(builtSet, rot);
    drawBandAtRest(rot);
    paint(true);
    lv_obj_invalidate(img);
  }

  // The middle: "Spin" before the first spin, else the option under the pointer.
  void paint(bool force = false) {
    int seg = spinning || spun ? under() : -1;
    float op = 0.12f;
    if (bloomStart) {
      float k = fminf(1, (millis() - bloomStart) / (float)BLOOM_MS);
      op = 0.12f + 0.25f * sinf(k * M_PI);
      if (k >= 1) bloomStart = 0;
      force = true;
    }
    if (!force && seg == shown && seg == tinted) return;
    bool textChanged = seg != shown;
    shown = seg;
    // Recoloring the halo and ring costs ~40 ms (compute + LVGL redraw). Mid-spin, segments
    // pass every frame, so recolor at most ~8 times a second; the label keeps up every frame.
    bool tint = force || (seg != tinted && (!spinning || millis() - tintedAt >= 120));
    if (tint) {
      tinted = seg;
      tintedAt = millis();
    }
    if (seg < 0) {
      if (tint) drawMiddle(0x4F8CFF, op, 0x3E4146);  // accent halo; ring white at 22%
      if (textChanged) {
        lv_label_set_text(label, "Spin");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(label, theme::ink(), 0);
      }
    } else {
      uint32_t c = COLORS[seg % 8];
      if (tint) drawMiddle(c, op, c);
      if (textChanged) {
        const char *t = SETS[builtSet].opts[seg];
        lv_point_t sz;
        lv_text_get_size(&sz, t, &lv_font_montserrat_48, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_label_set_text(label, t);
        lv_obj_set_style_text_font(label, sz.x > 270 ? &lv_font_montserrat_36 : &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(c), 0);
      }
    }
    const char *subText = spinning ? "" : ui::upper(SETS[builtSet].name);
    if (strcmp(lv_label_get_text(sub), subText)) lv_label_set_text(sub, subText);
    if (!tint) return;  // the label redraws itself
    if (spinning) {
      invalidateMiddle(img);
      copyZone();  // the thin ring sits outside LVGL's part while spinning
    } else {
      lv_area_t a = {CEN0, CEN0, CEN0 + CEN - 1, CEN0 + CEN - 1};
      lv_obj_invalidate_area(img, &a);
    }
  }

  void rotate(int dir) override {
    if (spinning) return;
    int s = constrain(setIdx() + dir, 0, NSETS - 1);
    if (s == setIdx()) return;
    store::setIntLater("wheelset", s);
    showSet();
  }

  void press() override {
    if (spinning) return;
    buildSpinTex(builtSet);
    spinning = true;
    spun = true;
    bloomStart = 0;
    from = rot;
    to = rot + 360 * 5 + (esp_random() % 3600) / 10.0f;
    spinStart = millis();
    bandFrames = 0;
    bandUs = 0;
  }

  void endSpin() {
    if (spinTex) {
      heap_caps_free(spinTex);
      spinTex = nullptr;
    }
    spinning = false;
  }

  void tick() override {
    if (spinning) {
      float k = fminf(1, (millis() - spinStart) / (float)SPIN_MS);
      rot = from + (to - from) * (1 - powf(1 - k, 4));
      if (k >= 1) {
        endSpin();
        rot = fmodf(rot, 360);
        Serial.printf("[wheel] spin: %d band frames (%.1f fps), %.1f ms each\n", (int)bandFrames,
                      bandFrames * 1000.0f / SPIN_MS, bandFrames ? bandUs / 1000.0f / bandFrames : 0.0f);
        layLabels(builtSet, rot);  // labels upright where they stopped
        drawBandAtRest(rot);
        bloomStart = millis();
        paint(true);
        lv_obj_invalidate(img);  // back to LVGL drawing everything
        return;
      }
      paint();
      uint32_t t = micros();
      drawSpinFrame(rot);
      bandUs += micros() - t;
      bandFrames++;
    } else if (bloomStart) {
      paint();
    }
  }
};

}  // namespace

Screen *makeWheelMode() { return new WheelMode(); }
String wheelSetName() { return SETS[setIdx()].name; }

void renderOrb();   // breathe.cpp
void renderCoin();  // play.cpp

// Boot prep, so first visits don't stall. On core 0 in the background: Breathe's orb, the coin,
// then the wheel's lookup tables (~3 s). Here on the UI core, before the backlight comes on:
// the saved set's label textures (~0.6 s; they draw text through LVGL, which only this core
// may use).
void wheelPrepare() {
  if (tablesStarted) return;
  tablesStarted = true;
  xTaskCreatePinnedToCore([](void *) {
    renderOrb();
    renderCoin();
    buildTables();
    vTaskDelete(nullptr);
  }, "wheelprep", 4096, nullptr, 1, nullptr, 0);
  allocTextures();
  lv_obj_t *c = lv_canvas_create(lv_layer_top());
  lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
  buildTextures(setIdx(), c);
  lv_obj_delete(c);
}
