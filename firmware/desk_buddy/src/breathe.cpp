// Breathe panel (renderBreathe/breatheMode in display.html). Rotate picks a pattern, press
// starts. The orb grows while breathing in and shrinks while breathing out; holds keep it still.
#include <atomic>
#include <esp_heap_caps.h>
#include <math.h>

#include "icons.h"
#include "modes.h"
#include "store.h"
#include "theme.h"
#include "sounds.h"

namespace {

struct Pattern {
  const char *name;
  int phases[4];  // in, hold, out, hold (seconds; 0 = skip)
};
const Pattern PATTERNS[] = {
  {"Box", {4, 4, 4, 4}}, {"4-7-8", {4, 7, 8, 0}}, {"Calm", {5, 0, 5, 0}}, {"Slow", {6, 0, 6, 0}},
};
constexpr int NPATTERNS = 4;
const char *const PHASE_NAMES[] = {"Breathe in", "Hold", "Breathe out", "Hold"};
constexpr int ORB = 298;          // .62 of the screen
constexpr float ORB_SMALL = 0.42f;

int patternIdx() { return constrain(store::getInt("breath", 0), 0, NPATTERNS - 1); }

String patternLine(const Pattern &p) {
  const char *n[] = {"In", "Hold", "Out", "Hold"};
  String s;
  for (int i = 0; i < 4; i++)
    if (p.phases[i]) {
      if (s.length()) s += " \xE2\x80\xA2 ";
      s += String(n[i]) + " " + p.phases[i];
    }
  return s;
}

// The orb: radial-gradient(circle at 50% 45%, rgba(122,170,255,.42), rgba(79,140,255,.12) 62%,
// rgba(79,140,255,.02)) with box-shadow 0 0 60px rgba(79,140,255,.2), blended over the screen
// color and dithered, rendered once into an RGB565 image. The session scales that image.
constexpr int GLOW = 60;
constexpr int IMG = ORB + 2 * GLOW;
uint16_t *orbBuf = nullptr;
lv_image_dsc_t orbImg;

const uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

std::atomic<int> orbState{0};  // 0 not started, 1 rendering, 2 ready

void renderOrbNow();

}  // namespace

// Render the orb once. The boot prep task calls this so the first visit to Breathe doesn't
// stall ~0.5 s; if the UI gets here first it renders itself, or waits for the task to finish.
void renderOrb() {
  int expect = 0;
  if (orbState.compare_exchange_strong(expect, 1)) {
    uint32_t t0 = millis();
    renderOrbNow();
    orbState = 2;
    Serial.printf("[breathe] orb in %u ms\n", (unsigned)(millis() - t0));
  }
  while (orbState != 2) delay(1);
}

namespace {

void renderOrbNow() {
  orbBuf = (uint16_t *)heap_caps_malloc(IMG * IMG * 2, MALLOC_CAP_SPIRAM);
  if (!orbBuf) {  // PSRAM ran out: makeOrb draws a plain circle instead
    Serial.println("[breathe] no room for the orb image");
    return;
  }
  // Work in 565 levels, starting from LVGL's own fill for the page color, so pixels where the
  // glow has faded to nothing match the background exactly (no visible square).
  const uint16_t flat = lv_color_to_u16(theme::screen());
  const float lvl[3] = {31, 63, 31};
  const float base[3] = {float(flat >> 11), float((flat >> 5) & 63), float(flat & 31)};
  const float c0[3] = {122, 170, 255}, c1[3] = {79, 140, 255};
  const float r = ORB / 2.0f, cx = IMG / 2.0f, cy = IMG / 2.0f;
  const float gx = cx, gy = GLOW + ORB * 0.45f;                      // gradient center
  const float gr = hypotf(ORB - (gx - GLOW), ORB - (gy - GLOW));    // to the farthest corner
  // box-shadow blur 60 = sigma 30, shifted so it reaches exactly 0 at the image's side edges.
  const float S2 = 2 * 30.0f * 30.0f, tail = expf(-(GLOW * GLOW) / S2);
  // The orb is mirror-symmetric left/right, so each pixel's color is computed once for both.
  for (int y = 0; y < IMG; y++) {
    if ((y & 7) == 7) vTaskDelay(1);  // runs on core 0 at boot: let its idle task run
    for (int x = 0; x < (IMG + 1) / 2; x++) {
      float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
      float col[3], a;
      if (d <= r + 0.5f) {
        float t = hypotf(x + 0.5f - gx, y + 0.5f - gy) / gr;
        float k = t < 0.62f ? t / 0.62f : (t - 0.62f) / 0.38f;
        const float *A = t < 0.62f ? c0 : c1;
        float aA = t < 0.62f ? 0.42f : 0.12f, aB = t < 0.62f ? 0.12f : 0.02f;
        k = fminf(1, k);
        for (int i = 0; i < 3; i++) col[i] = A[i] + (c1[i] - A[i]) * k;
        a = (aA + (aB - aA) * k) * fminf(1, r + 0.5f - d);  // anti-aliased edge
        // Glow under the antialiased edge.
        float g = 0.2f * fmaxf(0, 1 - fminf(1, r + 0.5f - d));
        a = fmaxf(a, g);
      } else {
        float o = d - r;
        a = 0.2f * fmaxf(0, (expf(-(o * o) / S2) - tail) / (1 - tail));
        for (int i = 0; i < 3; i++) col[i] = c1[i];
      }
      float v[3];
      for (int i = 0; i < 3; i++) v[i] = base[i] + (col[i] / 255.0f * lvl[i] - base[i]) * a;
      for (int xx : {x, IMG - 1 - x}) {
        float th = (BAYER4[y & 3][xx & 3] + 0.5f) / 16.0f;
        auto q = [&](int i) { return (uint16_t)fmaxf(0, fminf(lvl[i], floorf(v[i] + th))); };
        orbBuf[y * IMG + xx] = a <= 0 ? flat : (uint16_t)((q(0) << 11) | (q(1) << 5) | q(2));
      }
    }
  }
  memset(&orbImg, 0, sizeof(orbImg));
  orbImg.header.magic = LV_IMAGE_HEADER_MAGIC;
  orbImg.header.cf = LV_COLOR_FORMAT_RGB565;
  orbImg.header.w = IMG;
  orbImg.header.h = IMG;
  orbImg.header.stride = IMG * 2;
  orbImg.data_size = IMG * IMG * 2;
  orbImg.data = (const uint8_t *)orbBuf;
}

lv_obj_t *makeOrb(lv_obj_t *root) {
  renderOrb();
  if (!orbBuf) {
    lv_obj_t *c = lv_obj_create(root);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, IMG * 62 / 100, IMG * 62 / 100);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x4F8CFF), 0);
    lv_obj_set_style_bg_opa(c, 70, 0);
    lv_obj_set_style_transform_pivot_x(c, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(c, LV_PCT(50), 0);
    lv_obj_center(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
  }
  lv_obj_t *img = lv_image_create(root);
  lv_image_set_src(img, &orbImg);
  lv_obj_center(img);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  return img;
}

void setOrbScale(lv_obj_t *orb, float s) {
  if (lv_obj_check_type(orb, &lv_image_class)) lv_image_set_scale(orb, (uint32_t)(256 * s));
  else lv_obj_set_style_transform_scale(orb, (int32_t)(256 * s), 0);  // the plain-circle stand-in
}

// ---------- the guided session ----------
struct BreatheMode : Screen {
  bool active() const override { return true; }
  const Pattern &p;
  lv_obj_t *orb = nullptr, *phaseLbl = nullptr, *countLbl = nullptr, *sub = nullptr;
  int phase = -1;
  uint32_t phaseStart = 0, started = 0;
  float scale = ORB_SMALL;
  int shownCount = -1, shownSec = -1;

  explicit BreatheMode(const Pattern &p) : p(p) {}

  void build(lv_obj_t *root) override {
    orb = makeOrb(root);
    setOrbScale(orb, scale);
    lv_obj_t *col = ui::column(root, 2);
    phaseLbl = ui::label(col, "", &lv_font_montserrat_34, theme::ink());
    countLbl = ui::label(col, "", &lv_font_montserrat_48, theme::ink());
    sub = ui::label(col, "", &lv_font_montserrat_20, theme::inkDim());
    lv_obj_t *r = ui::row(col);
    ui::select(ui::chip(r, "Finish", ui::Tone::Stop, [](lv_event_t *e) {
      ((BreatheMode *)lv_event_get_user_data(e))->finish();
    }, this), true);
    if (!started) started = millis();
    shownCount = shownSec = -1;
    if (phase < 0) next(0);
    tick();
  }

  void next(int i) {
    int k = i % 4;
    while (!p.phases[k]) k = (k + 1) % 4;
    phase = k;
    phaseStart = millis();
    if (k == 0) sounds::play(sounds::BREATHE_IN);
    else if (k == 2) sounds::play(sounds::BREATHE_OUT);
    lv_label_set_text(phaseLbl, PHASE_NAMES[k]);
  }

  void tick() override {
    uint32_t len = p.phases[phase] * 1000UL;
    uint32_t t = millis() - phaseStart;
    if (t >= len) { next(phase + 1); t = 0; len = p.phases[phase] * 1000UL; }
    // Orb: in grows .42 -> 1, out shrinks 1 -> .42 (ease in-out), holds stay put.
    float k = (float)t / len;
    float e = k < 0.5f ? 2 * k * k : 1 - powf(-2 * k + 2, 2) / 2;
    float target = phase == 0 ? ORB_SMALL + (1 - ORB_SMALL) * e
                 : phase == 2 ? 1 - (1 - ORB_SMALL) * e
                 : phase == 1 ? 1.0f : ORB_SMALL;
    if (fabsf(target - scale) > 0.004f) { scale = target; setOrbScale(orb, scale); }
    int count = (int)max<uint32_t>(1, (len - t + 999) / 1000);
    uint32_t s = (millis() - started) / 1000;
    if (count != shownCount || (int)s != shownSec) {  // only touch labels when they change
      shownCount = count;
      shownSec = s;
      lv_label_set_text_fmt(countLbl, "%d", count);
      lv_label_set_text_fmt(sub, "%s \xE2\x80\xA2 %lu:%02lu", p.name, (unsigned long)(s / 60), (unsigned long)(s % 60));
    }
  }

  void finish() {
    uint32_t s = (millis() - started + 500) / 1000;
    String msg = s >= 60 ? String((s + 30) / 60) + " min of breathing" : String(s) + "s of breathing";
    ui::toast(msg.c_str(), 1);
    app::drop(this);
  }
  void press() override { finish(); }
};

// ---------- the panel ----------
struct BreathePanel : Panel {
  lv_obj_t *topLbl = nullptr, *name = nullptr, *line = nullptr;

  BreathePanel() : Panel("breathe", "Breathe", &icon_breathe) {}

  void build(lv_obj_t *root) override {
    makeOrb(root);
    topLbl = ui::top(root, "");
    lv_obj_t *col = ui::column(root, 6);
    lv_obj_t *l = ui::label(col, "BREATHE", &lv_font_montserrat_18, theme::inkDim());
    lv_obj_set_style_text_letter_space(l, 4, 0);
    name = ui::label(col, "", &lv_font_montserrat_48, theme::ink());
    line = ui::label(col, "", &lv_font_montserrat_20, theme::inkDim());
    lv_obj_t *r = ui::row(col);
    ui::select(ui::chip(r, "Start", ui::Tone::Primary, [](lv_event_t *e) {
      ((BreathePanel *)lv_event_get_user_data(e))->press();
    }, this), true);
    update();
  }
  void update() {
    int i = patternIdx();
    lv_label_set_text_fmt(topLbl, "%d OF %d", i + 1, NPATTERNS);
    lv_label_set_text(name, PATTERNS[i].name);
    lv_label_set_text(line, patternLine(PATTERNS[i]).c_str());
  }
  void rotate(int dir) override {
    store::setIntLater("breath", constrain(patternIdx() + dir, 0, NPATTERNS - 1));
    update();
  }
  void press() override { app::push(new BreatheMode(PATTERNS[patternIdx()])); }
};

}  // namespace

Panel *makeBreathePanel() { return new BreathePanel(); }
