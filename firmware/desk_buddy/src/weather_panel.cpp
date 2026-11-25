// Weather panel (renderWeather in display.html). Shows only once Wi-Fi is set up.
//   Hourly: the next 24 hours as a temperature curve; rotate scrubs the hour.
//   7-day:  seven day cells (icon, range bar, high); rotate scrubs the day.
//   Opens on 7-day. Press switches Hourly / 7-day. Tapping the curve, a day or a mode jumps straight there.
// The head (icon, temperature, condition, detail line) follows the scrubbed hour or day, and a
// soft glow at the top of the screen takes the condition's color (painted per pixel with
// dithering, like Home's ring, on the painter task; see painter.h).
#include <esp_heap_caps.h>
#include <math.h>

#include "tz.h"
#include "app.h"
#include "dither.h"
#include "icons.h"
#include "knob.h"
#include "net.h"
#include "painter.h"
#include "theme.h"
#include "weather.h"
#include "wx_icons.h"

LV_FONT_DECLARE(font_temp_88);

namespace {

using weather::Cond;

constexpr int W = 480;
constexpr int HOURS = 24, DAYS = 7;
// Layout (px from the top of the screen). Sized up from the faux display, like the other panels.
constexpr int HEAD_Y = 112, COND_Y = 214, SUB_Y = 247, STRIP_Y = 286, STRIP_H = 110, MODES_Y = 404;
constexpr int CURVE_W = 360;
constexpr int CELL_W = 44, CELL_GAP = 4, BAR_H = 26;

const char *SEP = " \xE2\x80\xA2 ";  // " • " (the built-in fonts have no middle dot)

const lv_image_dsc_t *wxIcon(int code, bool day, bool big) {
  static const lv_image_dsc_t *const LG[] = {&wx_clear_day_lg, &wx_partly_day_lg, &wx_cloudy_lg, &wx_fog_lg,
                                             &wx_rain_lg, &wx_snow_lg, &wx_storm_lg};
  static const lv_image_dsc_t *const SM[] = {&wx_clear_day_sm, &wx_partly_day_sm, &wx_cloudy_sm, &wx_fog_sm,
                                             &wx_rain_sm, &wx_snow_sm, &wx_storm_sm};
  Cond c = weather::condOf(code);
  if (!day && c == weather::CLEAR) return big ? &wx_clear_night_lg : &wx_clear_night_sm;
  if (!day && c == weather::PARTLY) return big ? &wx_partly_night_lg : &wx_partly_night_sm;
  return big ? LG[c] : SM[c];
}

String hourLabel(int h) { return tz::hourText(h); }

// "Rain 40%", or "Snow 40%" when the forecast is snow (the chance covers either). Nothing
// when the forecast has no chance for it.
String rainText(int code, int chance) {
  if (chance == weather::NO_VALUE) return "";
  const char *what = weather::condOf(code) == weather::SNOW ? "Snow" : "Rain";
  return chance ? String(what) + " " + chance + "%" : String("No ") + (weather::condOf(code) == weather::SNOW ? "snow" : "rain");
}

struct tm localOf(time_t t) {
  struct tm l;
  localtime_r(&t, &l);
  return l;
}

// ---------- background glow (display.html .wx-bg) ----------

struct Glow {
  uint32_t color;
  float opa, reach;  // reach: fraction of the distance to the farthest corner
};
Glow glowFor(int code, bool day) {
  if (!day) return {0x5A50BE, 0.28f, 0.62f};
  switch (weather::condOf(code)) {
    case weather::CLEAR: return {0x4F8CFF, 0.32f, 0.62f};
    case weather::PARTLY: return {0x6E8CBE, 0.28f, 0.62f};
    case weather::RAIN:
    case weather::STORM: return {0x465F8C, 0.38f, 0.65f};
    default: return {0x8C96AA, 0.22f, 0.62f};  // cloudy, fog, snow
  }
}

void paintGlow(uint16_t *bgBuf, const Glow &g) {
  using namespace dither;
  const RGB screen = rgb(0x0B0D11);
  const RGB inner = mix(screen, rgb(g.color), g.opa);
  const float cx = W / 2.0f, cy = W * 0.18f;
  const float far = hypotf(W / 2.0f, W - cy);
  const float r = far * g.reach;
  for (int y = 0; y < W; y++) {
    if ((y & 7) == 7) vTaskDelay(1);  // let core 0 idle: its task watchdog panics after 5 s
    for (int x = 0; x < W; x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      float d = sqrtf(dx * dx + dy * dy);  // hardware sqrt; hypotf is several times slower
      RGB c = d >= r ? screen : mix(inner, screen, d / r);
      bgBuf[y * W + x] = to565(c, x, y);
    }
  }
}

struct WeatherPanel : Panel {
  // Opens on 7-day every visit (2026-11-11); Hourly is a press away.
  enum Mode { HOURLY, DAILY } mode = DAILY;
  bool away = false;  // set while off screen, so the next visit starts on 7-day again
  int idx[2] = {0, 0};  // scrubbed hour / day, 0 = now / today
  KnobSpeed speed;

  weather::Data d;
  bool have = false;
  int h0 = 0, d0 = 0, nHours = 0, nDays = 0;  // first upcoming hour/day in d, and how many
  int lo = 0, hi = 0;                         // curve range
  uint32_t shownVer = 0;
  weather::State shownState = weather::State::Off;
  uint32_t lastCheck = 0;

  lv_obj_t *bg = nullptr, *caption = nullptr, *headIcon = nullptr, *temp = nullptr;
  lv_obj_t *cond = nullptr, *sub = nullptr, *curve = nullptr, *dayRow = nullptr;
  lv_obj_t *modeSeg[2] = {nullptr, nullptr};
  std::vector<lv_obj_t *> cells;
  BgCanvas bgc;

  WeatherPanel() : Panel("weather", "Weather", &icon_weather) {}

  // Off screen: when new weather arrives, paint the glow the panel will open with ("Now"), so
  // it's there on the first visit instead of fading in ~1.4 s later.
  uint32_t warmVer = 0;
  void background() override {
    if (app::currentPanel() != this) away = true;  // not just covered by an alert or sub-screen
    bgc.tick();  // swap in a finished paint (only touches the canvas if one exists)
    if (weather::version() == warmVer) return;
    warmVer = weather::version();
    weather::Data w;
    if (!weather::get(w)) return;
    bgc.syncFirst = false;
    setGlow(glowFor(w.now.code, w.now.day));
  }
  bool visible() const override { return net::configured(); }

  // ---------- build ----------

  void build(lv_obj_t *root) override {
    if (away) { mode = DAILY; away = false; }  // a fresh visit, not a rebuild while on screen
    shownVer = weather::version();
    shownState = weather::state();
    have = weather::get(d);
    cells.clear();
    curve = dayRow = nullptr;

    bgc.syncFirst = false;  // first visit: the glow fades in a moment later instead of stalling the swipe
    bg = bgc.build(root);  // the last painted glow, right away

    if (!have) {
      buildEmpty(root);
      return;
    }
    h0 = d.firstHour();
    d0 = d.firstDay();
    nHours = min(HOURS, (int)d.hours.size() - h0);
    nDays = min(DAYS, (int)d.days.size() - d0);
    idx[HOURLY] = constrain(idx[HOURLY], 0, max(0, nHours - 1));
    idx[DAILY] = constrain(idx[DAILY], 0, max(0, nDays - 1));
    lo = 999, hi = -999;
    for (int i = 0; i < nHours; i++) {
      lo = min(lo, (int)d.hours[h0 + i].temp);
      hi = max(hi, (int)d.hours[h0 + i].temp);
    }

    caption = ui::top(root, "");

    lv_obj_t *head = lv_obj_create(root);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_SIZE_CONTENT, 100);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, HEAD_Y);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 14, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    headIcon = lv_image_create(head);
    temp = ui::label(head, "", &font_temp_88, theme::ink());

    cond = ui::label(root, "", &lv_font_montserrat_24, theme::ink());
    lv_obj_align(cond, LV_ALIGN_TOP_MID, 0, COND_Y);
    sub = ui::label(root, "", &lv_font_montserrat_20, theme::inkDim());
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(sub, 420);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, SUB_Y);

    buildCurve(root);
    buildDays(root);
    buildModes(root);
    update();
  }

  void buildEmpty(lv_obj_t *root) {
    String loc = weather::locationLabel();
    int comma = loc.indexOf(',');
    ui::top(root, (comma > 0 ? loc.substring(0, comma) : loc).c_str());
    lv_obj_t *col = ui::column(root, 10);
    String a, b;
    switch (weather::state()) {
      case weather::State::NotFound:
        if (!loc.length()) {
          a = "No location yet";
          b = "Pick a city in Settings > Weather";
          break;
        }
        a = "Couldn't find \"" + loc + "\"";
        b = "Pick it again in Settings > Weather";
        break;
      case weather::State::Failed:
        a = "Couldn't get the weather";
        b = "Trying again in a few minutes";
        break;
      case weather::State::Waiting:
        a = "Waiting for Wi-Fi";
        b = "The forecast loads once it's connected";
        break;
      default:
        a = "Getting the weather...";
        break;
    }
    lv_obj_t *l = ui::label(col, a.c_str(), &lv_font_montserrat_28, theme::ink());
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, 360);
    if (b.length()) {
      l = ui::label(col, b.c_str(), &lv_font_montserrat_20, theme::inkDim());
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
      lv_obj_set_width(l, 340);
    }
    setGlow({0x0B0D11, 0, 0.62f});  // plain
  }

  // Glows are keyed by color; the painter keeps the last one and coalesces fast scrubbing.
  void setGlow(const Glow &g) {
    bgc.request(g.color, [g](uint16_t *buf) {
      uint32_t t0 = millis();
      paintGlow(buf, g);
      Serial.printf("[weather] glow %06lX, %lu ms\n", (unsigned long)g.color, (unsigned long)(millis() - t0));
    });
  }

  void buildCurve(lv_obj_t *root) {
    curve = lv_obj_create(root);
    lv_obj_remove_style_all(curve);
    lv_obj_set_size(curve, CURVE_W, STRIP_H);
    lv_obj_align(curve, LV_ALIGN_TOP_MID, 0, STRIP_Y);
    lv_obj_add_flag(curve, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(curve, drawCurve, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(curve, [](lv_event_t *e) {
      auto *self = (WeatherPanel *)lv_event_get_user_data(e);
      lv_point_t p;
      lv_indev_get_point(lv_indev_active(), &p);
      lv_area_t a;
      lv_obj_get_coords(self->curve, &a);
      self->idx[HOURLY] = constrain((int)lroundf(self->hourAt(p.x - a.x1)), 0, self->nHours - 1);
      self->update();
    }, LV_EVENT_CLICKED, this);
  }

  // Curve geometry, in px inside the curve object.
  // CURVE_PAD leaves room for the "Now" label centered under the first point.
  static constexpr int CURVE_PAD = 28;
  float xOf(int i) const { return CURVE_PAD + (float)i / max(1, nHours - 1) * (CURVE_W - 2 * CURVE_PAD); }
  float hourAt(int x) const { return (x - CURVE_PAD) / (float)(CURVE_W - 2 * CURVE_PAD) * max(1, nHours - 1); }
  float yOf(int t) const {
    float span = max(4, hi - lo + 4);
    return 10 + (1 - (t - lo + 2) / span) * (STRIP_H - 44);
  }

  static void drawCurve(lv_event_t *e) {
    auto *self = (WeatherPanel *)lv_event_get_user_data(e);
    if (self->nHours < 2) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->curve, &a);
    auto P = [&](int i) {
      return lv_point_precise_t{a.x1 + self->xOf(i), a.y1 + self->yOf(self->d.hours[self->h0 + i].temp)};
    };

    // Scrubbed hour: a faint vertical line.
    int s = self->idx[HOURLY];
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = theme::accent();
    ln.opa = 150;
    ln.width = 2;
    ln.p1 = {a.x1 + self->xOf(s), (float)a.y1 + 4};
    ln.p2 = {a.x1 + self->xOf(s), (float)a.y2 - 30};
    lv_draw_line(layer, &ln);

    // Temperature curve (amber, like display.html's polyline).
    ln.color = theme::today();
    ln.opa = 217;
    ln.width = 3;
    ln.round_start = ln.round_end = 1;
    for (int i = 0; i + 1 < self->nHours; i++) {
      ln.p1 = P(i);
      ln.p2 = P(i + 1);
      lv_draw_line(layer, &ln);
    }

    // Marker on the scrubbed hour.
    lv_point_precise_t m = P(s);
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = lv_color_white();
    dot.bg_opa = LV_OPA_COVER;
    dot.border_color = theme::accent();
    dot.border_width = 3;
    dot.border_opa = LV_OPA_COVER;
    lv_area_t da = {(int32_t)(m.x - 8), (int32_t)(m.y - 8), (int32_t)(m.x + 8), (int32_t)(m.y + 8)};
    lv_draw_rect(layer, &dot, &da);

    // Hour labels under the curve: Now, +6 h, +12 h, +18 h.
    lv_draw_label_dsc_t lb;
    lv_draw_label_dsc_init(&lb);
    lb.font = &lv_font_montserrat_16;
    lb.color = theme::inkDim();
    lb.align = LV_TEXT_ALIGN_CENTER;
    for (int k : {0, 6, 12, 18}) {
      if (k >= self->nHours) break;
      String t = k == 0 ? String("Now") : hourLabel(localOf(self->d.hours[self->h0 + k].at).tm_hour);
      lb.text = t.c_str();
      lb.text_local = 1;
      int32_t x = a.x1 + (int32_t)self->xOf(k);
      lv_area_t la = {x - 40, a.y2 - 22, x + 40, a.y2};
      lv_draw_label(layer, &lb, &la);
    }
  }

  void buildDays(lv_obj_t *root) {
    dayRow = lv_obj_create(root);
    lv_obj_remove_style_all(dayRow);
    lv_obj_set_size(dayRow, LV_SIZE_CONTENT, STRIP_H);
    lv_obj_align(dayRow, LV_ALIGN_TOP_MID, 0, STRIP_Y);
    lv_obj_set_flex_flow(dayRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dayRow, CELL_GAP, 0);
    lv_obj_remove_flag(dayRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dayRow, LV_OBJ_FLAG_CLICKABLE);
    int wlo = 999, whi = -999;
    for (int i = 0; i < nDays; i++) {
      wlo = min(wlo, (int)d.days[d0 + i].lo);
      whi = max(whi, (int)d.days[d0 + i].hi);
    }
    float span = max(1, whi - wlo);
    for (int i = 0; i < nDays; i++) {
      const weather::Day &day = d.days[d0 + i];
      lv_obj_t *c = lv_obj_create(dayRow);
      lv_obj_remove_style_all(c);
      lv_obj_set_size(c, CELL_W, STRIP_H);
      lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(c, 3, 0);
      lv_obj_set_style_radius(c, 14, 0);
      lv_obj_set_style_bg_color(c, theme::accent(), 0);
      lv_obj_set_style_border_color(c, theme::accent(), 0);
      lv_obj_set_style_border_width(c, 1, 0);
      lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_user_data(c, (void *)(intptr_t)i);
      lv_obj_add_event_cb(c, [](lv_event_t *e) {
        auto *self = (WeatherPanel *)lv_event_get_user_data(e);
        self->idx[DAILY] = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
        self->update();
      }, LV_EVENT_CLICKED, this);

      char name[8];
      if (i == 0) strcpy(name, "Today");
      else {
        struct tm t = localOf(day.date + 12 * 3600);  // noon, so the device's time zone can't shift the day
        strftime(name, sizeof(name), "%a", &t);
        name[2] = 0;  // "Mo", "Tu" (display.html slices to two letters)
      }
      lv_obj_t *n = ui::label(c, name, i == 0 ? &lv_font_montserrat_12 : &lv_font_montserrat_16, theme::inkDim());
      lv_obj_set_height(n, 19);
      lv_obj_t *ic = lv_image_create(c);
      lv_image_set_src(ic, wxIcon(day.code, true, false));
      lv_obj_t *bar = lv_obj_create(c);
      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, 6, BAR_H);
      lv_obj_set_style_radius(bar, 3, 0);
      lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
      lv_obj_set_style_bg_opa(bar, 20, 0);
      lv_obj_t *fill = lv_obj_create(bar);
      lv_obj_remove_style_all(fill);
      int top = (int)lroundf((1 - (day.hi - wlo) / span) * BAR_H);
      int bot = (int)lroundf((day.lo - wlo) / span * BAR_H);
      lv_obj_set_size(fill, 6, max(4, BAR_H - top - bot));
      lv_obj_set_pos(fill, 0, min(top, BAR_H - 4));
      lv_obj_set_style_radius(fill, 3, 0);
      lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(fill, theme::today(), 0);
      lv_obj_set_style_bg_grad_color(fill, theme::accent(), 0);
      lv_obj_set_style_bg_grad_dir(fill, LV_GRAD_DIR_VER, 0);
      String t = String(day.hi) + "\xC2\xB0";
      ui::label(c, t.c_str(), &lv_font_montserrat_18, theme::ink());
      cells.push_back(c);
    }
  }

  void buildModes(lv_obj_t *root) {
    lv_obj_t *pill = lv_obj_create(root);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, MODES_Y);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(pill, 4, 0);
    lv_obj_set_style_pad_column(pill, 4, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pill, 13, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    const char *names[2] = {"Hourly", "7-day"};
    for (int m = 0; m < 2; m++) {
      lv_obj_t *s = lv_obj_create(pill);
      lv_obj_remove_style_all(s);
      lv_obj_set_size(s, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_style_pad_hor(s, 20, 0);
      lv_obj_set_style_pad_ver(s, 6, 0);
      lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(s, theme::accent(), 0);
      lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(s, 6);
      lv_obj_set_user_data(s, (void *)(intptr_t)m);
      lv_obj_add_event_cb(s, [](lv_event_t *e) {
        auto *self = (WeatherPanel *)lv_event_get_user_data(e);
        self->mode = (Mode)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
        self->update();
      }, LV_EVENT_CLICKED, this);
      ui::label(s, names[m], &lv_font_montserrat_18, theme::ink());
      modeSeg[m] = s;
    }
  }

  // ---------- update ----------

  void update() {
    if (!have) return;
    String place = d.place.length() ? d.place : weather::locationLabel();
    String line, condText;
    int code, t;
    bool day;
    if (mode == HOURLY) {
      int i = idx[HOURLY];
      const weather::Hour &h = d.hours[h0 + i];
      if (i == 0) {
        // "Now" uses the current conditions, not the hour's forecast.
        code = d.now.code;
        day = d.now.day;
        t = d.now.temp;
        const weather::Day &today = d.days[d0];
        condText = String(weather::condName(code)) + SEP + "H " + today.hi + "\xC2\xB0 L " + today.lo + "\xC2\xB0";
        // "NW 12 km/h": with the unit, plainly wind. A humidity or wind the forecast left out is left out.
        // (2026-11-10) No "Feels", just humidity and wind.
        line = "";
        if (d.now.humidity != weather::NO_VALUE) line += String("Humidity ") + d.now.humidity + "%";
        if (d.now.wind != weather::NO_VALUE)
          line += String(line.length() ? SEP : "") + (*d.now.windDir ? String(d.now.windDir) + " " : String("Wind ")) + d.now.wind + (d.celsius ? " km/h" : " mph");
      } else {
        code = h.code;
        day = h.day;
        t = h.temp;
        condText = weather::condName(code);
        line = rainText(h.code, h.rain);
      }
    } else {
      int i = idx[DAILY];
      const weather::Day &dd = d.days[d0 + i];
      code = dd.code;
      day = true;
      t = dd.hi;
      condText = String(weather::condName(code)) + SEP + "H " + dd.hi + "\xC2\xB0 L " + dd.lo + "\xC2\xB0";
      line = rainText(dd.code, dd.rain);
    }
    // The place only (2026-11-09: the graph shows the hour; Home shows stale data's age).
    // A long name ("Rancho Santa Margarita") is shortened to fit the circle.
    String cap = place;
    auto width = [](const String &s) {
      lv_point_t p;
      lv_text_get_size(&p, ui::upper(s.c_str()), &lv_font_montserrat_18, 3, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      return p.x;
    };
    for (int n = place.length() - 1; n > 3 && width(cap) > 380; n--) {
      String head = place.substring(0, n);
      head.trim();
      cap = head + "...";
    }
    lv_label_set_text(caption, ui::upper(cap.c_str()));
    lv_image_set_src(headIcon, wxIcon(code, day, true));
    lv_label_set_text_fmt(temp, "%d\xC2\xB0", t);
    lv_label_set_text(cond, condText.c_str());
    lv_label_set_text(sub, line.c_str());

    if (mode == HOURLY) {
      lv_obj_remove_flag(curve, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(dayRow, LV_OBJ_FLAG_HIDDEN);
      lv_obj_invalidate(curve);
    } else {
      lv_obj_add_flag(curve, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(dayRow, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < (int)cells.size(); i++) {
      bool on = i == idx[DAILY];
      lv_obj_set_style_bg_opa(cells[i], on ? 36 : LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(cells[i], on ? 153 : LV_OPA_TRANSP, 0);
    }
    for (int m = 0; m < 2; m++) {
      lv_obj_set_style_bg_opa(modeSeg[m], m == mode ? 56 : LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(lv_obj_get_child(modeSeg[m], 0), m == mode ? theme::ink() : theme::inkDim(), 0);
    }

    setGlow(glowFor(code, day));
  }

  void rotate(int dir) override {
    if (!have) return;
    int n = mode == HOURLY ? nHours : nDays;
    // Hours: a fast turn moves 3 at a time. Days (only 8) stay 1 per click.
    int step = mode == HOURLY && speed.fast(dir) ? 3 : 1;
    int v = constrain(idx[mode] + dir * step, 0, max(0, n - 1));
    if (v == idx[mode]) return;
    idx[mode] = v;
    update();
  }

  void press() override {
    if (!have) {
      weather::refresh();
      return;
    }
    mode = mode == HOURLY ? DAILY : HOURLY;
    update();
  }

  void tick() override {
    bgc.tick();
    if (weather::version() != shownVer || (!have && weather::state() != shownState)) {
      app::refresh();
      return;
    }
    if (have && millis() - lastCheck > 30000) {
      lastCheck = millis();
      if (d.firstHour() != h0 || d.firstDay() != d0) {  // an hour (or a day) went by
        app::refresh();
        return;
      }
    }
  }
};

}  // namespace

Panel *makeWeatherPanel() { return new WeatherPanel(); }
