// World panel (LISTS.world, worldRing, cityDetail, addCityFlow in display.html): a list of
// cities with their local time, and around the screen's edge a 24-hour ring (noon at the top,
// midnight at the bottom, night shaded) with a dot per city at its local time. The blue dot is
// here; the selected city's dot is white and larger.
#include <math.h>

#include <algorithm>

#include "clock.h"
#include "icons.h"
#include "list_wheel.h"
#include "modes.h"
#include "store.h"
#include "theme.h"
#include "tz.h"

namespace {

constexpr float U = 4.8f;          // display.html viewBox unit in px
constexpr float RING_R = 46.9f * U;
constexpr float RING_W = 2.4f * U;
const char *SEP = " \xE2\x80\xA2 ";  // " • "

// ---------- saved cities (CITIES indexes) ----------
std::vector<int> cities;
String citySel;  // select this one when the list next builds (a just-added city)

void loadCities() {
  cities.clear();
  String s = store::getStr("cities", "0,8,14");  // New York, London, Sydney (display.html sample)
  int start = 0;
  while (start < (int)s.length()) {
    int comma = s.indexOf(',', start);
    if (comma < 0) comma = s.length();
    int i = s.substring(start, comma).toInt();
    if (comma > start && i >= 0 && i < tz::CITY_COUNT) cities.push_back(i);
    start = comma + 1;
  }
}

void saveCities() {
  String s;
  for (size_t i = 0; i < cities.size(); i++) s += (i ? "," : "") + String(cities[i]);
  store::setStr("cities", s);
}

bool hasCity(int i) {
  for (int c : cities)
    if (c == i) return true;
  return false;
}

// ---------- time math ----------
struct Info {
  tz::Local l;
  String time, rel, day;
};

Info info(int city) {
  time_t now = time(nullptr);
  Info r;
  r.l = tz::at(tz::CITIES[city].posix, now);
  tz::Local here = tz::at(tz::devicePosix(), now);
  r.time = tz::timeText(r.l.hour, r.l.min);
  int32_t diff = (tz::offset(tz::CITIES[city].posix, now) - tz::offset(tz::devicePosix(), now)) / 60;
  if (diff == 0) {
    r.rel = "Same time";
  } else {
    int32_t a = abs(diff);
    char b[16];
    if (a % 60) snprintf(b, sizeof(b), "%c%ld:%02ld h", diff > 0 ? '+' : '-', (long)(a / 60), (long)(a % 60));
    else snprintf(b, sizeof(b), "%c%ld h", diff > 0 ? '+' : '-', (long)(a / 60));
    r.rel = b;
  }
  r.day = r.l.day > here.day ? "Tomorrow" : r.l.day < here.day ? "Yesterday" : "Today";
  return r;
}

lv_point_precise_t polar(float deg, float r, float cx = 240, float cy = 240) {
  float a = deg * M_PI / 180;
  return {cx + r * sinf(a), cy - r * cosf(a)};
}

// Degrees clockwise from the top for an hour of the day (noon at the top).
float hourAngle(float h) { return (h - 12) / 24 * 360; }

// ---------- the ring ----------
struct Ring {
  lv_obj_t *track = nullptr, *here = nullptr, *sel = nullptr;
  std::vector<lv_obj_t *> dots;

  static lv_obj_t *dot(lv_obj_t *root, int d, lv_color_t c, bool glow) {
    lv_obj_t *o = lv_obj_create(root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, theme::screen(), 0);
    lv_obj_set_style_border_width(o, 3, 0);
    if (glow) {
      lv_obj_set_style_shadow_color(o, c, 0);
      lv_obj_set_style_shadow_width(o, 12, 0);
      lv_obj_set_style_shadow_opa(o, LV_OPA_60, 0);
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
  }

  // Day and night arcs. Drawn on request only; nothing invalidates this full-screen object.
  static void drawTrack(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = 240;
    arc.center.y = 240;
    arc.radius = (uint16_t)(RING_R + RING_W / 2);
    arc.width = (int32_t)RING_W;
    // LVGL angles: 0 = 3 o'clock, clockwise; kept positive.
    auto lv = [](float h) { return (int32_t)lroundf(hourAngle(h) - 90 + 720) % 360; };
    arc.start_angle = lv(7);   // day, 7 AM to 7 PM: rgba(245,185,66,.3) over the screen
    arc.end_angle = lv(19);
    arc.color = lv_color_hex(0x51411F);
    lv_draw_arc(layer, &arc);
    arc.start_angle = lv(19);  // night: rgba(120,110,230,.5)
    arc.end_angle = lv(7);
    arc.color = lv_color_hex(0x423E7C);
    lv_draw_arc(layer, &arc);
  }

  void build(lv_obj_t *root) {
    track = lv_obj_create(root);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 480, 480);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(track, drawTrack, LV_EVENT_DRAW_MAIN, nullptr);
    dots.clear();
    for (size_t i = 0; i < cities.size(); i++) dots.push_back(dot(root, 15, lv_color_hex(0x9AA2C4), false));
    here = dot(root, 19, theme::accent(), true);
    sel = dot(root, 24, lv_color_white(), true);
  }

  // Above the list, so the list's bottom fade (a full-width dark gradient) doesn't darken the
  // ring there (2026-11-11).
  void raise() {
    lv_obj_move_foreground(track);
    for (lv_obj_t *d : dots) lv_obj_move_foreground(d);
    lv_obj_move_foreground(here);
    lv_obj_move_foreground(sel);
  }

  static void place(lv_obj_t *o, float hour) {
    lv_point_precise_t p = polar(hourAngle(hour), RING_R);
    int32_t w = lv_obj_get_style_width(o, LV_PART_MAIN);
    int32_t x = lroundf(p.x) - w / 2, y = lroundf(p.y) - w / 2;
    if (lv_obj_get_x(o) != x || lv_obj_get_y(o) != y) lv_obj_set_pos(o, x, y);  // moving redraws
  }

  void update(int selCity) {
    time_t now = time(nullptr);
    for (size_t i = 0; i < dots.size() && i < cities.size(); i++) {
      tz::Local l = tz::at(tz::CITIES[cities[i]].posix, now);
      place(dots[i], l.hour + l.min / 60.0f);
      bool hide = cities[i] == selCity;
      if (hide != lv_obj_has_flag(dots[i], LV_OBJ_FLAG_HIDDEN)) {
        if (hide) lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    tz::Local h = tz::at(tz::devicePosix(), now);
    place(here, h.hour + h.min / 60.0f);
    if (selCity >= 0) {
      tz::Local l = tz::at(tz::CITIES[selCity].posix, now);
      place(sel, l.hour + l.min / 60.0f);
      lv_obj_remove_flag(sel, LV_OBJ_FLAG_HIDDEN);
    } else if (!lv_obj_has_flag(sel, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_add_flag(sel, LV_OBJ_FLAG_HIDDEN);
    }
  }
};

// ---------- one city: an analog clock ----------
constexpr int CLOCK = 180;

struct CityDetail : DetailMode {
  int city;
  lv_obj_t *face = nullptr, *timeLbl = nullptr, *subLbl = nullptr;
  int shownSec = -1;

  explicit CityDetail(int city) : city(city) {
    gap = 6;
    body = [this]() -> std::vector<Line> {
      return {{tz::CITIES[this->city].name, &lv_font_montserrat_30, theme::ink()}};
    };
    extra = [this](lv_obj_t *col) {
      face = lv_obj_create(col);
      lv_obj_remove_style_all(face);
      lv_obj_set_size(face, CLOCK, CLOCK);
      lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(face, drawFace, LV_EVENT_DRAW_MAIN, this);
      timeLbl = ui::label(col, "", &lv_font_montserrat_34, theme::ink());
      subLbl = ui::label(col, "", &lv_font_montserrat_20, theme::inkDim());
      shownSec = -1;
      update();
    };
    controls = [this]() -> std::vector<Control> {
      int c = this->city;
      return {
        {"Back", ui::Tone::Normal, []() { app::pop(); }},
        {"Remove", ui::Tone::Stop, [this, c]() {
          std::vector<int> next;
          for (int x : cities)
            if (x != c) next.push_back(x);
          cities = next;
          saveCities();
          ui::toast("City removed");
          app::drop(this);
        }},
      };
    };
  }

  static void drawFace(lv_event_t *e) {
    auto *self = (CityDetail *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->face, &a);
    const float cx = a.x1 + CLOCK / 2.0f, cy = a.y1 + CLOCK / 2.0f, k = CLOCK / 100.0f;

    lv_draw_arc_dsc_t ring;  // ".face-c": faint fill, thin rim
    lv_draw_arc_dsc_init(&ring);
    ring.center.x = lroundf(cx);
    ring.center.y = lroundf(cy);
    ring.start_angle = 0;
    ring.end_angle = 360;
    ring.radius = (uint16_t)(48 * k);
    ring.width = ring.radius;
    ring.color = lv_color_hex(0x121418);
    lv_draw_arc(layer, &ring);
    ring.width = 1;
    ring.color = lv_color_hex(0x2F3235);
    lv_draw_arc(layer, &ring);

    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.round_start = 1;
    ln.round_end = 1;
    ln.color = lv_color_hex(0x85878A);  // white at 50%
    ln.width = 3;
    for (int i = 0; i < 12; i++) {
      ln.p1 = polar(i * 30, 40 * k, cx, cy);
      ln.p2 = polar(i * 30, 45 * k, cx, cy);
      lv_draw_line(layer, &ln);
    }

    tz::Local l = tz::at(tz::CITIES[self->city].posix, time(nullptr));
    auto hand = [&](float deg, float len, float w, lv_color_t c) {
      ln.p1 = {cx, cy};
      ln.p2 = polar(deg, len * k, cx, cy);
      ln.width = (int32_t)lroundf(w * k);
      ln.color = c;
      lv_draw_line(layer, &ln);
    };
    hand((l.hour % 12 + l.min / 60.0f) * 30, 24, 3.2f, theme::ink());
    hand((l.min + l.sec / 60.0f) * 6, 36, 2, theme::ink());
    hand(l.sec * 6, 40, 0.9f, theme::accent());

    lv_draw_arc_dsc_t hub;
    lv_draw_arc_dsc_init(&hub);
    hub.center.x = lroundf(cx);
    hub.center.y = lroundf(cy);
    hub.start_angle = 0;
    hub.end_angle = 360;
    hub.radius = (uint16_t)(2.2f * k + 1);
    hub.width = hub.radius;
    hub.color = theme::accent();
    lv_draw_arc(layer, &hub);
  }

  void update() {
    if (!face) return;
    time_t now = time(nullptr);
    int sec = now % 60;
    if (sec == shownSec) return;
    shownSec = sec;
    lv_obj_invalidate(face);
    Info i = info(city);
    if (strcmp(lv_label_get_text(timeLbl), i.time.c_str())) lv_label_set_text(timeLbl, i.time.c_str());
    String sub = i.rel + SEP + i.day;
    if (strcmp(lv_label_get_text(subLbl), sub.c_str())) lv_label_set_text(subLbl, sub.c_str());
  }

  void build(lv_obj_t *root) override {
    face = nullptr;
    DetailMode::build(root);
  }
  void tick() override { update(); }
};

void addCityFlow() {
  std::vector<int> opts;
  for (int i = 0; i < tz::CITY_COUNT; i++)
    if (!hasCity(i)) opts.push_back(i);
  if (opts.empty()) { ui::toast("All cities added"); return; }
  std::vector<String> labels;
  for (int i : opts) labels.push_back(String(tz::CITIES[i].name) + SEP + info(i).time);
  app::push(new PickerMode("Add city", "", labels, -1, [opts](int k) {
    int c = opts[k];
    cities.push_back(c);
    saveCities();
    citySel = String(c);
    ui::toast((String("Added ") + tz::CITIES[c].name).c_str(), 1);
  }));
}

// ---------- the World list ----------
struct WorldPanel : Panel {
  ListWheel wheel;
  Ring ring;
  lv_obj_t *topLbl = nullptr;
  String selId;
  uint32_t lastRefresh = 0;

  WorldPanel() : Panel("world", "World", &icon_world) { loadCities(); }

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    // Time zone order, west to east (by the offset in effect now, so DST can reorder them),
    // then by name for cities sharing a zone.
    std::vector<int> order = cities;
    time_t now = time(nullptr);
    std::sort(order.begin(), order.end(), [now](int x, int y) {
      int32_t ox = tz::offset(tz::CITIES[x].posix, now), oy = tz::offset(tz::CITIES[y].posix, now);
      return ox != oy ? ox < oy : strcmp(tz::CITIES[x].name, tz::CITIES[y].name) < 0;
    });
    for (int c : order) {
      Info i = info(c);
      v.push_back({String(c), tz::CITIES[c].name, i.time, i.rel + SEP + i.day, ListItem::Normal});
    }
    v.push_back({"add", "+ Add city", "", "", ListItem::Add});
    return v;
  }

  int selCity() {
    const ListItem *it = wheel.selectedItem();
    return it && it->id != "add" ? it->id.toInt() : -1;
  }

  void build(lv_obj_t *root) override {
    if (citySel.length()) { selId = citySel; citySel = ""; }
    ring.build(root);
    wheel = ListWheel();
    wheel.showBar = false;  // the 24-hour ring is on the edge (no "N / M" counter either; 2026-11-11)
    wheel.build(root, 0.8f);
    ring.raise();
    std::vector<ListItem> its = items();
    int start = 0;
    for (int i = 0; i < (int)its.size(); i++)
      if (its[i].id == selId) start = i;
    wheel.setItems(its);
    wheel.jump(start);
    wheel.onPick = [this](int) { pick(); };
    wheel.onChange = [this]() { remember(); };
    topLbl = ui::top(root, "");
    remember();
  }

  void remember() {
    if (const ListItem *it = wheel.selectedItem()) selId = it->id;
    tz::Local h = tz::at(tz::devicePosix(), time(nullptr));
    String t = String("HERE ") + tz::timeText(h.hour, h.min);
    if (strcmp(lv_label_get_text(topLbl), t.c_str())) lv_label_set_text(topLbl, t.c_str());
    ring.update(selCity());
  }

  void pick() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "add") addCityFlow();
    else app::push(new CityDetail(it->id.toInt()));
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { pick(); }
  void tick() override {
    wheel.tick();
    if (millis() - lastRefresh >= 1000) {  // times tick over each minute
      lastRefresh = millis();
      wheel.setItems(items());
      remember();
    }
  }
};

}  // namespace

Panel *makeWorldPanel() { return new WorldPanel(); }
