// Play panel (LISTS.play in display.html) and its games: coin, dice, ring pong, reflex. The
// Spin the wheel was removed (archive/spin_wheel). Pong and Reflex are games: swipes are ignored, reminders wait, and
// the idle return to Home leaves them alone.
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>

#include "board.h"
#include "dither.h"
#include "icons.h"
#include "list_wheel.h"
#include "modes.h"
#include "store.h"
#include "theme.h"
#include "sounds.h"

LV_FONT_DECLARE(font_clock_96);


namespace {

const char *SEP = " \xE2\x80\xA2 ";  // " • "
uint32_t rnd(uint32_t n) { return esp_random() % n; }

void setText(lv_obj_t *l, const String &t) {
  if (strcmp(lv_label_get_text(l), t.c_str())) lv_label_set_text(l, t.c_str());
}

// A row of chips that the knob moves between (game over screens).
struct ChipRow {
  std::vector<lv_obj_t *> chips;
  std::vector<std::function<void()>> fns;
  int sel = 0;
  void build(lv_obj_t *col, std::vector<Control> ctls) {
    chips.clear();
    fns.clear();
    lv_obj_t *r = ui::row(col);
    for (int i = 0; i < (int)ctls.size(); i++) {
      lv_obj_t *c = ui::chip(r, ctls[i].label.c_str(), ctls[i].tone, [](lv_event_t *e) {
        auto *self = (ChipRow *)lv_event_get_user_data(e);
        lv_obj_t *t = (lv_obj_t *)lv_event_get_current_target(e);
        for (int k = 0; k < (int)self->chips.size(); k++)
          if (self->chips[k] == t) { self->sel = k; self->fire(); }
      }, this);
      ui::select(c, i == sel);
      chips.push_back(c);
      fns.push_back(ctls[i].fn);
    }
  }
  void rotate(int dir) {
    int n = constrain(sel + dir, 0, (int)chips.size() - 1);
    if (n == sel || chips.empty()) return;
    ui::select(chips[sel], false);
    sel = n;
    ui::select(chips[sel], true);
  }
  void fire() {
    if (sel < (int)fns.size()) { auto f = fns[sel]; f(); }
  }
};

lv_obj_t *backChip(lv_obj_t *parent) {
  return ui::chip(ui::row(parent), "Back", ui::Tone::Normal, [](lv_event_t *) { app::pop(); }, nullptr);
}

// A full-screen invisible target behind everything: a tap anywhere acts like the knob.
lv_obj_t *tapAnywhere(lv_obj_t *root, Screen *s, lv_event_code_t code = LV_EVENT_SHORT_CLICKED) {
  lv_obj_t *o = lv_obj_create(root);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 480, 480);
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);  // hold still reaches the screen
  lv_obj_add_event_cb(o, [](lv_event_t *e) {
    auto *s = (Screen *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) s->pressDown(millis());
    else s->press();
  }, code, s);
  return o;
}

// ======================= coin =======================
// A gold coin: radial-gradient(circle at 35% 30%, #ffe9a3, #d9a93a 60%, #a8781c) with a faint
// inner rim, rendered once (dithered) and squashed vertically to flip.
constexpr int COIN = 170, COIN_PAD = 4, COIN_IMG = COIN + 2 * COIN_PAD;
uint16_t *coinBuf = nullptr;
lv_image_dsc_t coinImg;

void renderCoinNow();

}  // namespace

// Render the coin once. The boot prep task calls this (like Breathe's orb) so the first flip
// doesn't stall ~0.2 s; if the UI gets here first it renders itself, or waits for the task.
std::atomic<int> coinState{0};  // 0 not started, 1 rendering, 2 ready
void renderCoin() {
  int expect = 0;
  if (coinState.compare_exchange_strong(expect, 1)) {
    renderCoinNow();
    coinState = 2;
  }
  while (coinState != 2) delay(1);
}

namespace {

void renderCoinNow() {
  coinBuf = (uint16_t *)heap_caps_malloc(COIN_IMG * COIN_IMG * 2, MALLOC_CAP_SPIRAM);
  if (!coinBuf) {  // PSRAM ran out: the coin is a plain gold circle
    Serial.println("[play] no room for the coin image");
    return;
  }
  using namespace dither;
  // Outside the coin, use LVGL's own 565 value for the page color; dithering it to the true
  // #0B0D11 averages lighter than LVGL's truncated fill and shows as a square.
  const uint16_t flat = lv_color_to_u16(theme::screen());
  const RGB bg = {float((flat >> 11) << 3), float(((flat >> 5) & 63) << 2), float((flat & 31) << 3)};
  const RGB c0 = rgb(0xFFE9A3), c1 = rgb(0xD9A93A), c2 = rgb(0xA8781C);
  const float r = COIN / 2.0f, cx = COIN_IMG / 2.0f, gx = COIN_PAD + COIN * 0.35f, gy = COIN_PAD + COIN * 0.30f;
  const float gr = hypotf(COIN_PAD + COIN - gx, COIN_PAD + COIN - gy);
  for (int y = 0; y < COIN_IMG; y++)
    for (int x = 0; x < COIN_IMG; x++) {
      float d = hypotf(x + 0.5f - cx, y + 0.5f - cx);
      float t = hypotf(x + 0.5f - gx, y + 0.5f - gy) / gr;
      RGB c = t < 0.6f ? mix(c0, c1, t / 0.6f) : mix(c1, c2, fminf(1, (t - 0.6f) / 0.4f));
      float rim = fmaxf(0, 1 - fabsf(d - (r - 3)) / 3);  // inset white ring at 25%
      c = mix(c, {255, 255, 255}, 0.25f * rim);
      float a = fmaxf(0, fminf(1, r + 0.5f - d));
      coinBuf[y * COIN_IMG + x] = a <= 0 ? flat : to565(mix(bg, c, a), x, y);
    }
  memset(&coinImg, 0, sizeof(coinImg));
  coinImg.header.magic = LV_IMAGE_HEADER_MAGIC;
  coinImg.header.cf = LV_COLOR_FORMAT_RGB565;
  coinImg.header.w = COIN_IMG;
  coinImg.header.h = COIN_IMG;
  coinImg.header.stride = COIN_IMG * 2;
  coinImg.data_size = COIN_IMG * COIN_IMG * 2;
  coinImg.data = (const uint8_t *)coinBuf;
}

struct CoinMode : Screen {
  lv_obj_t *coin = nullptr, *face = nullptr, *res = nullptr, *tally = nullptr;
  bool busy = false, heads = true;
  char shownFace = 'H';
  String result;
  uint32_t t0 = 0;
  static constexpr uint32_t FLIP_MS = 900;

  void build(lv_obj_t *root) override {
    renderCoin();
    tapAnywhere(root, this);
    ui::top(root, "Flip a coin");
    lv_obj_t *col = ui::column(root, 6, true);
    lv_obj_t *box = lv_obj_create(col);  // fixed box: the coin grows while flipping
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, COIN_IMG, COIN_IMG);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    if (coinBuf) {
      coin = lv_image_create(box);
      lv_image_set_src(coin, &coinImg);
    } else {
      coin = lv_obj_create(box);
      lv_obj_remove_style_all(coin);
      lv_obj_set_size(coin, COIN_IMG * 9 / 10, COIN_IMG * 9 / 10);
      lv_obj_set_style_radius(coin, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(coin, lv_color_hex(0xE0B04A), 0);
      lv_obj_set_style_bg_opa(coin, LV_OPA_COVER, 0);
      lv_obj_set_style_transform_pivot_x(coin, LV_PCT(50), 0);
      lv_obj_set_style_transform_pivot_y(coin, LV_PCT(50), 0);
    }
    lv_obj_center(coin);
    lv_obj_remove_flag(coin, LV_OBJ_FLAG_CLICKABLE);
    char f[2] = {shownFace, 0};
    face = ui::label(box, f, &lv_font_montserrat_48, lv_color_hex(0x5A4210));
    lv_obj_center(face);
    res = ui::label(col, result.c_str(), &lv_font_montserrat_32, theme::ink());
    lv_obj_set_height(res, lv_font_get_line_height(&lv_font_montserrat_32));
    tally = ui::label(col, "", &lv_font_montserrat_20, theme::inkDim());
    backChip(col);
    update();
  }
  void scale(float x, float y) {
    if (coinBuf) {
      lv_image_set_scale_x(coin, (uint32_t)x);
      lv_image_set_scale_y(coin, (uint32_t)y);
    } else {
      lv_obj_set_style_transform_scale_x(coin, (int32_t)x, 0);
      lv_obj_set_style_transform_scale_y(coin, (int32_t)y, 0);
    }
  }
  void update() {
    setText(tally, String("Heads ") + store::getInt("coinh", 0) + SEP + "Tails " + store::getInt("coint", 0));
  }
  void press() override {
    if (busy) return;
    busy = true;
    sounds::play(sounds::COIN_FLIP);
    heads = rnd(2);
    t0 = millis();
    lv_obj_add_flag(face, LV_OBJ_FLAG_HIDDEN);
    setText(res, "");
  }
  void tick() override {
    if (!busy) return;
    float k = fminf(1, (millis() - t0) / (float)FLIP_MS);
    float e = 1 - powf(1 - k, 3);                     // ease out
    float ang = e * 1800 * M_PI / 180;               // five turns
    float s = k < 0.4f ? 1 + 0.25f * k / 0.4f : 1.25f - 0.25f * (k - 0.4f) / 0.6f;
    scale(256 * s, 256 * s * fmaxf(0.04f, fabsf(cosf(ang))));
    if (k >= 1) {
      busy = false;
      shownFace = heads ? 'H' : 'T';
      char f[2] = {shownFace, 0};
      lv_label_set_text(face, f);
      lv_obj_remove_flag(face, LV_OBJ_FLAG_HIDDEN);
      scale(256, 256);
      result = heads ? "Heads" : "Tails";
      setText(res, result);
      const char *key = heads ? "coinh" : "coint";
      store::setInt(key, store::getInt(key, 0) + 1);
      update();
    }
  }
};

// ======================= dice =======================
struct DiceSet { const char *name; int n, sides; };
const DiceSet DICE[] = {
  {"1d6", 1, 6}, {"2d6", 2, 6}, {"3d6", 3, 6}, {"d4", 1, 4}, {"d8", 1, 8},
  {"d10", 1, 10}, {"d12", 1, 12}, {"d20", 1, 20}, {"2d20", 2, 20},
};
constexpr int NDICE = sizeof(DICE) / sizeof(DICE[0]);
int diceIdx() { return constrain(store::getInt("diceset", 0), 0, NDICE - 1); }

const uint8_t PIPS[7][6][2] = {
  {}, {{15, 15}}, {{9, 9}, {21, 21}}, {{9, 9}, {15, 15}, {21, 21}},
  {{9, 9}, {21, 9}, {9, 21}, {21, 21}}, {{9, 9}, {21, 9}, {15, 15}, {9, 21}, {21, 21}},
  {{9, 8}, {21, 8}, {9, 15}, {21, 15}, {9, 22}, {21, 22}},
};

uint32_t dieColor(int sides) {
  switch (sides) {
    case 4: return 0x3FCF7F;
    case 8: return 0xA66BFF;
    case 10: return 0xFF9A3C;
    case 12: return 0xFF5FD2;
    default: return 0x4F8CFF;
  }
}

// Polygon dice (display.html dieSvg): the outline in the die's color over a 16% fill.
struct PolyDie {
  int sides;
  static void corners(int sides, float size, lv_point_precise_t *out, int &n, float ox, float oy) {
    const float k = size / 30.0f;
    auto poly = [&](int m, float r, float rot) {
      n = m;
      for (int i = 0; i < m; i++) {
        float a = (rot + i * 360.0f / m) * M_PI / 180;
        out[i] = {ox + (15 + r * cosf(a)) * k, oy + (15 + r * sinf(a)) * k};
      }
    };
    switch (sides) {
      case 4: poly(3, 14.5f, -90); break;
      case 8: poly(4, 14.5f, -90); break;
      case 10:
        n = 4;
        out[0] = {ox + 15 * k, oy + 0.8f * k};
        out[1] = {ox + 28.6f * k, oy + 11.5f * k};
        out[2] = {ox + 15 * k, oy + 29.2f * k};
        out[3] = {ox + 1.4f * k, oy + 11.5f * k};
        break;
      case 12: poly(5, 14.5f, -90); break;
      default: poly(6, 14.5f, -90); break;
    }
  }
  static void draw(lv_event_t *e) {
    lv_obj_t *o = (lv_obj_t *)lv_event_get_current_target(e);
    int sides = (int)(intptr_t)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float size = lv_area_get_width(&a), k = size / 30.0f;
    lv_point_precise_t p[6];
    int n = 0;
    corners(sides, size, p, n, a.x1, a.y1);
    lv_color_t c = lv_color_hex(dieColor(sides));
    lv_point_precise_t mid = {a.x1 + size / 2, a.y1 + size / 2};
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = c;
    tri.opa = 41;  // 16%
    for (int i = 0; i < n; i++) {
      tri.p[0] = mid;
      tri.p[1] = p[i];
      tri.p[2] = p[(i + 1) % n];
      lv_draw_triangle(layer, &tri);
    }
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = c;
    ln.width = (int32_t)lroundf(1.1f * k);
    ln.round_start = ln.round_end = 1;
    for (int i = 0; i < n; i++) {
      ln.p1 = p[i];
      ln.p2 = p[(i + 1) % n];
      lv_draw_line(layer, &ln);
    }
    if (sides == 20) {  // the front face: a triangle inside the hexagon, lines to the corners
      lv_point_precise_t in[3];
      for (int i = 0; i < 3; i++) {
        float ang = (-90 + i * 120) * M_PI / 180;
        in[i] = {a.x1 + (15 + 9 * cosf(ang)) * k, a.y1 + (15 + 9 * sinf(ang)) * k};
      }
      ln.width = max<int32_t>(1, (int32_t)lroundf(0.6f * k));
      ln.opa = 179;
      for (int i = 0; i < 3; i++) {
        ln.p1 = in[i];
        ln.p2 = in[(i + 1) % 3];
        lv_draw_line(layer, &ln);
      }
      const int pairs[6][2] = {{0, 0}, {1, 1}, {2, 1}, {3, 2}, {4, 2}, {5, 0}};
      ln.opa = 128;
      for (auto &pr : pairs) {
        ln.p1 = p[pr[0]];
        ln.p2 = in[pr[1]];
        lv_draw_line(layer, &ln);
      }
    }
  }
};

struct DiceMode : Screen {
  lv_obj_t *strip = nullptr, *row = nullptr, *res = nullptr;
  int vals[3] = {3, 5, 1};
  bool rolling = false, rolled = false;
  uint32_t rollStart = 0, lastShuffle = 0;
  std::vector<lv_obj_t *> dice, nums;

  void build(lv_obj_t *root) override {
    tapAnywhere(root, this);
    // Set strip at the top: the current set as a pill, its neighbours dim; tap one to switch.
    strip = ui::row(root, 10);
    lv_obj_set_width(strip, 360);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_align(strip, LV_ALIGN_TOP_MID, 0, 28);
    int cur = diceIdx();
    for (int i = cur - 1; i <= cur + 1; i++) {
      if (i < 0 || i >= NDICE) {
        lv_obj_t *sp = lv_obj_create(strip);  // keep the current one centered
        lv_obj_remove_style_all(sp);
        lv_obj_set_size(sp, 80, 1);
        continue;
      }
      lv_obj_t *c = lv_obj_create(strip);
      lv_obj_remove_style_all(c);
      lv_obj_set_size(c, 80, 40);
      lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
      if (i == cur) {
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, theme::accent(), 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x13203A), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
      }
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_user_data(c, (void *)(intptr_t)i);
      lv_obj_add_event_cb(c, [](lv_event_t *e) {
        auto *self = (DiceMode *)lv_event_get_user_data(e);
        self->setDice((int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e)));
      }, LV_EVENT_CLICKED, this);
      lv_obj_t *l = ui::label(c, DICE[i].name, &lv_font_montserrat_20, i == cur ? theme::ink() : theme::inkFaint());
      lv_obj_center(l);
    }
    lv_obj_t *col = ui::column(root, 10);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, 20);
    row = ui::row(col, 18);
    lv_obj_set_style_pad_top(row, 0, 0);
    const DiceSet &D = DICE[cur];
    dice.clear();
    nums.clear();
    for (int i = 0; i < D.n; i++) {
      int sz = D.sides == 6 ? (D.n == 1 ? 144 : D.n == 2 ? 115 : 96) : (D.n == 1 ? 173 : 130);
      lv_obj_t *d = lv_obj_create(row);
      lv_obj_remove_style_all(d);
      lv_obj_set_size(d, sz, sz);
      lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
      if (D.sides == 6) {
        lv_obj_set_style_radius(d, sz / 5, 0);
        lv_obj_set_style_bg_color(d, theme::ink(), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
      } else {
        lv_obj_add_event_cb(d, PolyDie::draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)D.sides);
        lv_obj_t *l = ui::label(d, "", D.n == 1 ? &lv_font_montserrat_48 : &lv_font_montserrat_36, theme::ink());
        lv_obj_align(l, LV_ALIGN_CENTER, 0, (int32_t)(D.sides == 4 ? sz * 0.12f : sz * 0.02f));
        nums.push_back(l);
      }
      dice.push_back(d);
    }
    res = ui::label(col, "", &lv_font_montserrat_32, theme::ink());
    lv_obj_set_height(res, lv_font_get_line_height(&lv_font_montserrat_32));
    backChip(col);
    show();
  }

  void show() {
    const DiceSet &D = DICE[diceIdx()];
    for (int i = 0; i < D.n && i < (int)dice.size(); i++) {
      int v = constrain(vals[i], 1, D.sides);
      if (D.sides == 6) {
        lv_obj_clean(dice[i]);
        int sz = lv_obj_get_style_width(dice[i], LV_PART_MAIN);
        float k = sz / 30.0f;
        for (int p = 0; p < v; p++) {
          lv_obj_t *pip = lv_obj_create(dice[i]);
          lv_obj_remove_style_all(pip);
          int pd = (int)lroundf(5.4f * k);
          lv_obj_set_size(pip, pd, pd);
          lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
          lv_obj_set_style_bg_color(pip, theme::screen(), 0);
          lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
          lv_obj_set_pos(pip, (int)lroundf(PIPS[v][p][0] * k - pd / 2.0f), (int)lroundf(PIPS[v][p][1] * k - pd / 2.0f));
        }
      } else {
        lv_label_set_text_fmt(nums[i], "%d", v);
      }
      // A little shake while rolling (display.html wobbles them).
      lv_obj_set_style_translate_x(dice[i], rolling ? (int)rnd(9) - 4 : 0, 0);
      lv_obj_set_style_translate_y(dice[i], rolling ? (int)rnd(9) - 4 : 0, 0);
    }
    String r;
    lv_color_t rc = theme::ink();
    if (rolled && !rolling) {
      int total = 0, hi = 0, lo = 99;
      for (int i = 0; i < D.n; i++) {
        total += vals[i];
        hi = max(hi, vals[i]);
        lo = min(lo, vals[i]);
      }
      if (D.sides == 20 && D.n == 1 && total == 20) { r = "Natural 20!"; rc = theme::today(); }
      else if (D.sides == 20 && D.n == 1 && total == 1) { r = "Natural 1"; rc = theme::overdue(); }
      else if (D.sides == 20 && D.n == 2) r = String("High ") + hi + SEP + "Low " + lo;
      else if (D.n > 1) r = String("Total ") + total;
    }
    setText(res, r);
    lv_obj_set_style_text_color(res, rc, 0);
  }

  void setDice(int i) {
    if (rolling) return;
    i = constrain(i, 0, NDICE - 1);
    if (i == diceIdx()) return;
    store::setIntLater("diceset", i);
    rolled = false;
    for (int &v : vals) v = 1 + rnd(DICE[i].sides);
    app::refresh();
  }
  void rotate(int dir) override { setDice(diceIdx() + dir); }
  bool swipe(int dir) override { setDice(diceIdx() + dir); return true; }
  void press() override {
    if (rolling) return;
    rolling = true;
    sounds::play(sounds::DICE_ROLL);
    rollStart = lastShuffle = millis();
  }
  void tick() override {
    if (!rolling) return;
    uint32_t now = millis();
    int sides = DICE[diceIdx()].sides;
    if (now - rollStart >= 650) {
      rolling = false;
      rolled = true;
      for (int &v : vals) v = 1 + rnd(sides);
      show();
    } else if (now - lastShuffle >= 80) {
      lastShuffle = now;
      for (int &v : vals) v = 1 + rnd(sides);
      show();
    }
  }
};

// ======================= ring pong =======================
// The paddle rides the inside of the round edge; the knob (or a finger anywhere) moves it.
// Each return speeds the ball up and shrinks the paddle. Units: the screen's radius = 1.
constexpr float R_WALL = 0.86f, BALL = 0.035f, UPX = 240;
constexpr float PAD_R = (R_WALL + 0.03f) * UPX, PAD_W = 0.045f * UPX;

float wrapPi(float a) {
  while (a > M_PI) a -= 2 * M_PI;
  while (a < -M_PI) a += 2 * M_PI;
  return a;
}

struct PongMode : Screen {
  lv_obj_t *paddle = nullptr, *ball = nullptr, *score = nullptr, *hud = nullptr;
  ChipRow over;
  float x = 0, y = 0, vx = 0, vy = 0, speed = 0.6f, pa = -M_PI / 2, hw = 0.5f;
  int pts = 0;
  bool served = false, missing = false, ended = false, paused = false, newBest = false;
  uint32_t last = 0;
  float drawnPa = 99, drawnHw = 0;
  // touch steering
  bool touching = false, steering = false;
  lv_point_t touchStart;

  bool isGame() const override { return true; }
  bool noSwipe() const override { return true; }

  void reset() {
    x = y = vx = vy = 0;
    speed = 0.6f;
    pa = -M_PI / 2;
    hw = 0.5f;
    pts = 0;
    served = missing = ended = paused = newBest = false;
  }

  // The paddle: an arc drawn by hand so only the strip it moves through is redrawn.
  static void drawPaddle(lv_event_t *e) {
    auto *self = (PongMode *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = 240;
    a.center.y = 240;
    float s = self->pa - self->hw, en = self->pa + self->hw;
    a.start_angle = (int32_t)lroundf(s * 180 / M_PI + 720) % 360;
    a.end_angle = (int32_t)lroundf(en * 180 / M_PI + 720) % 360;
    a.rounded = 1;
    a.color = theme::accent();
    a.width = (int32_t)(PAD_W + 10);  // glow
    a.radius = (uint16_t)(PAD_R + a.width / 2);
    a.opa = 50;
    lv_draw_arc(layer, &a);
    a.width = (int32_t)PAD_W;
    a.radius = (uint16_t)(PAD_R + PAD_W / 2);
    a.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &a);
  }

  void invalidatePaddle(float p, float h) {
    // Bounding box of the arc from p-h to p+h, padded for the width and glow.
    float minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (int i = 0; i <= 16; i++) {
      float t = p - h + 2 * h * i / 16;
      float px = 240 + PAD_R * cosf(t), py = 240 + PAD_R * sinf(t);
      minX = fminf(minX, px); maxX = fmaxf(maxX, px);
      minY = fminf(minY, py); maxY = fmaxf(maxY, py);
    }
    lv_area_t a = {(int32_t)minX - 24, (int32_t)minY - 24, (int32_t)maxX + 24, (int32_t)maxY + 24};
    lv_obj_invalidate_area(paddle, &a);
  }

  void build(lv_obj_t *root) override {
    // Touch is read directly in tick (steer or tap). This catcher keeps a finger held still
    // mid-rally from counting as the hold that backs out of the game.
    lv_obj_t *catcher = lv_obj_create(root);
    lv_obj_remove_style_all(catcher);
    lv_obj_set_size(catcher, 480, 480);
    lv_obj_t *track = lv_obj_create(root);  // the faint ring the paddle rides
    lv_obj_remove_style_all(track);
    int tr = (int)(PAD_R + PAD_W / 2) * 2;
    lv_obj_set_size(track, tr + 4, tr + 4);
    lv_obj_center(track);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(track, 7, 0);
    lv_obj_set_style_border_color(track, lv_color_hex(0x1C1E22), 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    score = ui::label(root, "", &font_clock_96, lv_color_hex(0x1C1E22));
    lv_obj_center(score);
    paddle = lv_obj_create(root);
    lv_obj_remove_style_all(paddle);
    lv_obj_set_size(paddle, 480, 480);
    lv_obj_remove_flag(paddle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(paddle, drawPaddle, LV_EVENT_DRAW_MAIN, this);
    ball = lv_obj_create(root);
    lv_obj_remove_style_all(ball);
    int bd = (int)lroundf(BALL * UPX * 2);
    lv_obj_set_size(ball, bd, bd);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ball, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ball, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(ball, lv_color_white(), 0);
    lv_obj_set_style_shadow_width(ball, 14, 0);
    lv_obj_set_style_shadow_opa(ball, LV_OPA_60, 0);
    lv_obj_remove_flag(ball, LV_OBJ_FLAG_CLICKABLE);
    hud = ui::column(root, 6);
    buildHud();
    placeBall();
    last = millis();
  }

  void buildHud() {
    lv_obj_clean(hud);
    if (ended) {
      ui::label(hud, newBest ? "New best" : "Game over", &lv_font_montserrat_22, theme::inkDim());
      ui::label(hud, String(pts).c_str(), &font_clock_96, theme::ink());
      ui::label(hud, (String("Best ") + store::getInt("bestpong", 0)).c_str(), &lv_font_montserrat_20, theme::inkDim());
      over.sel = 0;
      // Chip actions only change state and ask for a rebuild: rebuilding here would delete the
      // chip while LVGL is still delivering its click.
      over.build(hud, {{"Again", ui::Tone::Primary, [this]() { reset(); app::refresh(); }},
                       {"Back", ui::Tone::Normal, []() { app::pop(); }}});
    } else if (!served) {
      ui::label(hud, "Ring pong", &lv_font_montserrat_22, theme::inkDim());
      ui::label(hud, "Press to serve", &lv_font_montserrat_34, theme::ink());
      int best = store::getInt("bestpong", 0);
      ui::label(hud, best ? (String("Best ") + best).c_str() : "Turn the knob to move", &lv_font_montserrat_20, theme::inkDim());
      backChip(hud);
    } else if (paused) {
      ui::label(hud, "Paused", &lv_font_montserrat_22, theme::inkDim());
      ui::label(hud, String(pts).c_str(), &font_clock_96, theme::ink());
      ui::label(hud, "Press or tap to keep playing", &lv_font_montserrat_20, theme::inkDim());
      backChip(hud);
    }
    lv_obj_set_flag(ball, LV_OBJ_FLAG_HIDDEN, !served || ended);
    setText(score, served && !ended && !paused ? String(pts) : String(""));
  }

  void placeBall() {
    int bd = (int)lroundf(BALL * UPX * 2);
    lv_obj_set_pos(ball, (int32_t)lroundf(240 + x * UPX - bd / 2.0f), (int32_t)lroundf(240 + y * UPX - bd / 2.0f));
  }

  void serve() {
    float a = pa + ((int)rnd(1000) / 1000.0f - 0.5f) * 0.6f;  // toward the paddle: a fair first return
    vx = cosf(a) * speed;
    vy = sinf(a) * speed;
    served = true;
    sounds::play(sounds::PONG_SERVE);
    buildHud();
  }

  void update(float dt) {
    if (!served || ended || paused) return;
    x += vx * dt;
    y += vy * dt;
    float r = hypotf(x, y);
    if (!missing && r >= R_WALL - BALL) {
      float ba = atan2f(y, x);
      float diff = wrapPi(ba - pa);
      if (fabsf(diff) <= hw + 0.05f) {
        // Return: back across the circle, angled by where it hit the paddle.
        float out = ba + M_PI - fmaxf(-1, fminf(1, diff / hw)) * 0.85f + ((int)rnd(1000) / 1000.0f - 0.5f) * 0.2f;
        speed = fminf(1.7f, speed * 1.06f);
        hw = fmaxf(0.2f, hw * 0.96f);
        vx = cosf(out) * speed;
        vy = sinf(out) * speed;
        float k = (R_WALL - BALL - 0.002f) / r;
        x *= k;
        y *= k;
        pts++;
        setText(score, String(pts));
        sounds::play(sounds::PONG_HIT);
      } else {
        missing = true;
        sounds::play(sounds::PONG_MISS);
      }
    }
    if (missing && r > 1.08f) {
      ended = true;
      int best = store::getInt("bestpong", 0);
      if (pts > best) { store::setInt("bestpong", pts); newBest = true; sounds::play(sounds::PONG_BEST); }
      buildHud();
    }
  }

  void steer(float d) {
    if (ended || paused) return;
    pa = wrapPi(pa + d);
  }

  // Knob acceleration: a slow click moves the paddle ~10 deg for fine aim; clicks arriving fast
  // (a quick spin) move it up to 4x as far, so half the ring is one flick of the wrist.
  uint32_t lastStep = 0;
  int lastDir = 0;
  void rotate(int dir) override {
    if (ended) { over.rotate(dir); return; }
    uint32_t now = millis(), dt = now - lastStep;
    float mult = 1;
    if (dir == lastDir && dt < 150) mult = constrain(1 + (150 - (int)dt) / 35.0f, 1.0f, 4.0f);
    lastStep = now;
    lastDir = dir;
    steer(dir * 0.17f * mult);
  }
  void press() override {
    if (ended) { over.fire(); return; }
    if (paused) { paused = false; buildHud(); return; }
    if (!served) serve();
  }

  // Touch: a drag steers (the paddle goes to the finger's angle); a tap mid-rally pauses.
  void watchTouch() {
    lv_indev_t *t = board::touchIndev();
    if (!t) return;
    bool down = lv_indev_get_state(t) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(t, &p);
    if (down && !touching) {
      touching = true;
      steering = false;
      touchStart = p;
    } else if (down) {
      if (!steering && (abs(p.x - touchStart.x) > 8 || abs(p.y - touchStart.y) > 8)) steering = true;
      if (steering && served && !ended && !paused) pa = atan2f(p.y - 240, p.x - 240);
    } else if (touching) {
      touching = false;
      if (steering) return;
      if (served && !ended && !paused) { paused = true; buildHud(); }
      else if (!ended) press();  // serve or resume (game over waits for its chips)
    }
  }

  void tick() override {
    uint32_t now = millis();
    float dt = fminf(0.033f, (now - last) / 1000.0f);
    last = now;
    watchTouch();
    update(dt);
    if (served && !ended) placeBall();
    if (pa != drawnPa || hw != drawnHw) {
      if (drawnPa < 90) invalidatePaddle(drawnPa, drawnHw);
      invalidatePaddle(pa, hw);
      drawnPa = pa;
      drawnHw = hw;
    }
  }
};

// ======================= reflex =======================
// Wait for green, press as fast as you can. Five rounds; average and best. Timing uses the
// moment the knob's contact closes (or the finger lands), from the moment the green frame was
// drawn.
struct ReflexMode : Screen {
  enum State { READY, WAIT, GO, EARLY, RESULT, DONE } state = READY;
  static constexpr int ROUNDS = 5;
  std::vector<int> times;
  uint32_t goAt = 0, t0 = 0;
  int lastMs = 0;
  bool newBest = false;
  State downIn = READY;  // the state the last press started in
  lv_obj_t *ring = nullptr, *glow = nullptr, *col = nullptr;
  ChipRow endRow;

  bool isGame() const override { return true; }
  bool noSwipe() const override { return true; }

  void build(lv_obj_t *root) override {
    tapAnywhere(root, this, LV_EVENT_PRESSED);
    glow = lv_obj_create(root);
    lv_obj_remove_style_all(glow);
    lv_obj_set_size(glow, 466, 466);
    lv_obj_center(glow);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(glow, 26, 0);
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_CLICKABLE);
    ring = lv_obj_create(root);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 451, 451);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 7, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    col = ui::column(root, 6);
    show();
  }

  void show() {
    // ".ring-glow": faint; .wait a little brighter; .go green with a glow; .bad red.
    uint32_t c = 0x1F2024;
    lv_opa_t g = 0;
    if (state == WAIT) c = 0x2D2F32;
    if (state == GO) { c = 0x3FCF7F; g = 70; }
    if (state == EARLY) { c = 0xFF6B6B; g = 40; }
    lv_obj_set_style_border_color(ring, lv_color_hex(c), 0);
    lv_obj_set_style_border_color(glow, lv_color_hex(c), 0);
    lv_obj_set_style_border_opa(glow, g, 0);
    lv_obj_clean(col);
    String round = String("Round ") + min((int)times.size() + 1, ROUNDS) + " of " + ROUNDS;
    switch (state) {
      case READY: {
        ui::label(col, "Reflex", &lv_font_montserrat_22, theme::inkDim());
        lv_obj_t *l = ui::label(col, "Press when the ring turns green", &lv_font_montserrat_30, theme::ink());
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, 320);
        int best = store::getInt("bestreflex", 0);
        ui::label(col, best ? (String("Best average ") + best + " ms").c_str() : "5 rounds", &lv_font_montserrat_20, theme::inkDim());
        backChip(col);
        break;
      }
      case WAIT:
        ui::label(col, round.c_str(), &lv_font_montserrat_22, theme::inkDim());
        ui::label(col, "Wait...", &lv_font_montserrat_48, theme::ink());
        break;
      case GO:
        ui::label(col, round.c_str(), &lv_font_montserrat_22, theme::inkDim());
        ui::label(col, "Now!", &lv_font_montserrat_48, theme::ok());
        break;
      case EARLY:
        ui::label(col, round.c_str(), &lv_font_montserrat_22, theme::inkDim());
        ui::label(col, "Too soon", &lv_font_montserrat_48, theme::overdue());
        ui::label(col, "Press to try again", &lv_font_montserrat_20, theme::inkDim());
        backChip(col);  // a way out between rounds (2026-11-10)
        break;
      case RESULT:
        ui::label(col, (String("Round ") + times.size() + " of " + ROUNDS).c_str(), &lv_font_montserrat_22, theme::inkDim());
        ui::label(col, String(lastMs).c_str(), &font_clock_96, theme::ink());
        ui::label(col, "ms", &lv_font_montserrat_22, theme::inkDim());
        ui::label(col, "Press for the next round", &lv_font_montserrat_20, theme::inkDim());
        backChip(col);
        break;
      case DONE: {
        int sum = 0, fast = 99999;
        for (int t : times) { sum += t; fast = min(fast, t); }
        int n = times.size(), avg = (sum + n / 2) / n;
        ui::label(col, newBest ? "New best" : "Average", &lv_font_montserrat_22, newBest ? theme::today() : theme::inkDim());
        ui::label(col, String(avg).c_str(), &font_clock_96, theme::ink());
        ui::label(col, "ms", &lv_font_montserrat_22, theme::inkDim());
        ui::label(col, (String("Fastest ") + fast + " ms" + SEP + "best " + store::getInt("bestreflex", 0) + " ms").c_str(),
                  &lv_font_montserrat_18, theme::inkDim());
        endRow.sel = 0;
        endRow.build(col, {{"Again", ui::Tone::Primary, [this]() {
                             times.clear();
                             state = WAIT;
                             goAt = millis() + 1200 + rnd(2600);
                             app::refresh();
                           }},
                           {"Back", ui::Tone::Normal, []() { app::pop(); }}});
        break;
      }
    }
  }

  void arm() {
    state = WAIT;
    goAt = millis() + 1200 + rnd(2600);
    show();
  }

  void pressDown(uint32_t at) override {
    downIn = state;
    switch (state) {
      case READY:
      case EARLY:
      case RESULT:
        arm();
        break;
      case WAIT:
        state = EARLY;
        sounds::play(sounds::REFLEX_EARLY);
        show();
        break;
      case GO: {
        lastMs = (int)(at - t0);
        if (lastMs < 0) lastMs = 0;
        times.push_back(lastMs);
        if ((int)times.size() >= ROUNDS) {
          int sum = 0;
          for (int t : times) sum += t;
          int avg = (sum + ROUNDS / 2) / ROUNDS;
          int best = store::getInt("bestreflex", 0);
          newBest = !best || avg < best;
          if (newBest) store::setInt("bestreflex", avg);
          state = DONE;
          if (newBest) sounds::play(sounds::PONG_BEST);
          else sounds::reflexResult(lastMs);
        } else {
          state = RESULT;
          sounds::reflexResult(lastMs);
        }
        show();
        break;
      }
      case DONE:
        break;  // the chips act on release (press)
    }
  }
  void press() override {
    if (state == DONE && downIn == DONE) endRow.fire();
  }
  void rotate(int dir) override {
    if (state == DONE) endRow.rotate(dir);
  }
  void tick() override {
    if (state == WAIT && (int32_t)(millis() - goAt) >= 0) {
      state = GO;
      show();
      lv_refr_now(nullptr);  // draw the green frame now, and time from when it's drawn
      t0 = millis();
      sounds::play(sounds::REFLEX_GO);
    }
  }
};

// ======================= the Play list =======================
struct PlayPanel : Panel {
  ListWheel wheel;
  lv_obj_t *counter = nullptr;
  String selId = "pong";  // first in the list (order: pong, dice, coin, reflex)
  uint32_t lastRefresh = 0;

  PlayPanel() : Panel("play", "Play", &icon_play) {}

  std::vector<ListItem> items() {
    int h = store::getInt("coinh", 0), t = store::getInt("coint", 0);
    int bp = store::getInt("bestpong", 0), br = store::getInt("bestreflex", 0);
    return {
      {"pong", "Ring pong", bp ? String("Best ") + bp : String(""), "The knob is the paddle"},
      {"dice", "Roll dice", "", DICE[diceIdx()].name},
      {"coin", "Flip a coin", "", h + t ? String("Heads ") + h + SEP + "Tails " + t : String("Heads or tails")},
      {"reflex", "Reflex", br ? String("Best ") + br + " ms" : String(""), "Press when the ring turns green"},
    };
  }

  void build(lv_obj_t *root) override {
    wheel = ListWheel();
    wheel.build(root);
    std::vector<ListItem> its = items();
    int start = 0;
    for (int i = 0; i < (int)its.size(); i++)
      if (its[i].id == selId) start = i;
    wheel.setItems(its);
    wheel.jump(start);
    wheel.onPick = [this](int) { pick(); };
    wheel.onChange = [this]() { remember(); };
    counter = ui::bottom(root, "");
    lv_obj_add_flag(counter, LV_OBJ_FLAG_HIDDEN);  // the scroll arc and the cut-off row replace "N / M"
    remember();
  }
  void remember() {
    if (const ListItem *it = wheel.selectedItem()) selId = it->id;
    setText(counter, String(wheel.selected() + 1) + " / " + String((int)items().size()));
  }
  void pick() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "coin") app::push(new CoinMode());
    else if (it->id == "dice") app::push(new DiceMode());
    else if (it->id == "pong") app::push(new PongMode());
    else app::push(new ReflexMode());
  }
  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { pick(); }
  void tick() override {
    wheel.tick();
    if (millis() - lastRefresh >= 1000) {
      lastRefresh = millis();
      wheel.setItems(items());
    }
  }
};

}  // namespace

Panel *makePlayPanel() { return new PlayPanel(); }
