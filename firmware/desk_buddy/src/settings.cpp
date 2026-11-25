// Settings panel (LISTS.settings and the "settings flows" in display.html), plus Brightness,
// which the faux display doesn't have but the real backlight needs.
#include "tz.h"
#include "autodim.h"
#include "board.h"
#include "clock.h"
#include "daynum.h"
#include "icons.h"
#include "list_wheel.h"
#include "net.h"
#include "modes.h"
#include "panels.h"
#include "reminders.h"
#include "store.h"
#include "theme.h"
#include "timer_face.h"
#include "weather.h"
#include "sounds.h"

namespace {

const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

int daysIn(int year, int month) {
  static const int D[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : D[month - 1];
}

String timeText(const struct tm &t) { return tz::timeText(t.tm_hour, t.tm_min); }

// ---------- Setup mode (Wi-Fi and the first weather location are set from the phone) ----------
// While this screen is up the device runs its own network; joining it opens the setup page.
struct SetupMode : Screen {
  bool active() const override { return true; }  // phone setup in progress
  enum Phase { Waiting, Connecting, Failed } phase = Waiting;
  int sel = 0;
  std::vector<lv_obj_t *> chips;
  std::vector<Control> ctls;

  SetupMode() { net::startSetup(); }
  ~SetupMode() override { net::stopSetup(); }

  void build(lv_obj_t *root) override {
    ui::top(root, "Setup mode");
    lv_obj_t *col = ui::column(root, 8);
    auto dim = [&](const char *t) {
      lv_obj_t *l = ui::label(col, t, &lv_font_montserrat_20, theme::inkDim());
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
      lv_obj_set_width(l, 330);
    };
    ctls.clear();
    if (phase == Waiting) {
      dim("On your phone, join");
      ui::label(col, net::apName().c_str(), &lv_font_montserrat_34, theme::accent());
      dim("The setup page opens by itself. Wi-Fi is optional.");
      ctls.push_back({"Done", ui::Tone::Primary, []() { app::pop(); }});
      if (net::configured())
        ctls.push_back({"Forget Wi-Fi", ui::Tone::Stop, []() { net::forget(); ui::toast("Wi-Fi forgotten"); app::refresh(); }});
    } else if (phase == Connecting) {
      ui::label(col, "Connecting to", &lv_font_montserrat_20, theme::inkDim());
      ui::label(col, net::ssid().c_str(), &lv_font_montserrat_34, theme::ink());
      dim("This takes a few seconds.");
    } else {
      ui::label(col, "Couldn't connect", &lv_font_montserrat_34, theme::overdue());
      dim((String("Check the password for ") + net::ssid() + " on the setup page and save again.").c_str());
      ctls.push_back({"Done", ui::Tone::Primary, []() { app::pop(); }});
    }
    chips.clear();
    sel = constrain(sel, 0, max(0, (int)ctls.size() - 1));
    if (!ctls.empty()) {
      lv_obj_t *r = ui::row(col);
      for (int i = 0; i < (int)ctls.size(); i++) {
        lv_obj_t *c = ui::chip(r, ctls[i].label.c_str(), ctls[i].tone, [](lv_event_t *e) {
          auto *self = (SetupMode *)lv_event_get_user_data(e);
          lv_obj_t *t = (lv_obj_t *)lv_event_get_current_target(e);
          for (int k = 0; k < (int)self->chips.size(); k++)
            if (self->chips[k] == t) { self->sel = k; self->press(); }
        }, this);
        ui::select(c, i == sel);
        chips.push_back(c);
      }
    }
  }
  void rotate(int dir) override {
    if (chips.empty()) return;
    int n = constrain(sel + dir, 0, (int)chips.size() - 1);
    if (n == sel) return;
    ui::select(chips[sel], false);
    sel = n;
    ui::select(chips[sel], true);
  }
  void press() override {
    if (sel < (int)ctls.size()) { auto fn = ctls[sel].fn; fn(); }
  }
  void tick() override {
    bool wifi;
    if (net::takeSaved(wifi)) {
      if (!wifi) { ui::toast("Running offline", 1); app::drop(this); return; }
      phase = Connecting;
      app::refresh();
      return;
    }
    if (phase == Connecting) {
      if (net::status() == net::Status::Connected) {
        ui::toast((String("Connected to ") + net::ssid()).c_str(), 1);
        sounds::play(sounds::WIFI_OK);
        app::drop(this);
      } else if (net::status() == net::Status::Failed) {
        phase = Failed;
        sounds::play(sounds::ERROR);
        app::refresh();
      }
    }
  }
};

// ---------- Brightness ----------
// Same face as Focus: the edge dial shows the level out of 100%, the big number in the middle.
// Rotate: 1% per slow click, jumps to the next multiple of 5 when turned fast. Press or tap
// Done to leave; the value is already applied and saved.
// "Auto resumes at 5:00 PM" while the dial's level holds (until the next phase change).
String resumeText() {
  time_t t = autodim::overrideEnds();
  if (!t) return "Auto resumes later";
  struct tm l;
  localtime_r(&t, &l);
  return "Auto resumes at " + tz::timeText(l.tm_hour, l.tm_min);
}

// The same dial for Settings > Auto brightness > Day max (the brightest auto goes) and
// Night min (the dimmest, 1-25%; the screen previews it while the dial is open).
enum class Dial { Bright, DayMax, NightMin };
struct BrightnessMode : Screen {
  TimerFace face;
  MinuteKnob knob;
  Dial kind;
  explicit BrightnessMode(Dial kind = Dial::Bright) : kind(kind) {}
  int hi() const { return kind == Dial::NightMin ? 25 : 100; }
  void build(lv_obj_t *root) override {
    std::vector<Control> c;
    c.push_back({"Done", ui::Tone::Primary, []() { app::pop(); }});
    face.onRing = [this](float f) { set(lroundf(f * hi())); };
    face.build(root, kind == Dial::DayMax ? "Day max" : kind == Dial::NightMin ? "Night min" : "Brightness", c, 0);
    face.dial.setSmooth(true);
    update();
  }
  int shown = -1;
  String shownSub;
  int level() const {
    if (kind == Dial::DayMax) return autodim::dayMax();
    if (kind == Dial::NightMin) return autodim::nightMin();
    return constrain((int)lroundf(autodim::current()), 1, 100);
  }
  void update() {
    int pct = level();
    if (pct != shown) {
      shown = pct;
      face.setTime(String(pct), "%");
      face.dial.set(pct / (float)hi(), false);  // Night min: the full ring is 25%
    }
    String sub = kind == Dial::DayMax ? "Brightest auto goes" : kind == Dial::NightMin ? "Dimmest at night"
               : !autodim::enabled() ? "Screen backlight"
               : autodim::overriding() ? resumeText() : "Auto brightness";
    if (sub != shownSub) {
      shownSub = sub;
      face.setSub(sub.c_str());
    }
  }
  void set(int pct) {
    pct = constrain(pct, 1, hi());
    if (kind == Dial::DayMax) autodim::setDayMax(pct);  // the screen follows within a second (daytime)
    else if (kind == Dial::NightMin) autodim::setNightMin(pct);
    else autodim::setManual(pct);
    update();
  }
  void rotate(int dir) override { set(knob.step(level(), dir, 1, hi())); }
  bool ownsTouch() const override { return face.ringDown; }
  void press() override { face.press(); }
  void tick() override {
    face.dial.tick();
    if (kind == Dial::NightMin) autodim::preview(autodim::nightMin());  // ends a second after leaving
    update();  // auto keeps moving the level; only changed text is redrawn
  }
  const char *tag() const override { return kind == Dial::DayMax ? "daymax" : kind == Dial::NightMin ? "nightmin" : "bright"; }
};

// ---------- Auto brightness: on/off, wake time, bedtime, day brightness ----------
// Times move in 30-minute steps and follow Settings > Time format (tz::timeText).
String minText(int m) { m = ((m % 1440) + 1440) % 1440; return tz::timeText(m / 60, m % 60); }

// One time field in half hours. Wake 4:00-11:00 AM; bedtime 8:00 PM-2:00 AM (half hours past
// midnight count on from 48 so the knob doesn't wrap).
// preview: what the time means (the hour before or after it).
void halfHourFlow(const char *title, int curMin, int lo, int hi, std::function<String(int)> preview,
                  std::function<void(int)> done) {
  int v = curMin / 30;
  if (v < lo) v += 48;
  std::vector<Field> f = {{title, constrain(v, lo, hi), lo, hi, nullptr, [](int v) { return minText(v * 30); }}};
  app::push(new FieldsMode(title, f, [preview](const std::vector<Field> &v) { return preview((v[0].value * 30) % 1440); },
                           [done](const std::vector<Field> &v) { done((v[0].value * 30) % 1440); }));
}

String phaseText(autodim::Phase p) {
  switch (p) {
    case autodim::MORNING: return "Brightening";
    case autodim::DAY: return "Daytime";
    case autodim::DUSK: return "Dimming for the evening";
    case autodim::EVENING: return "Evening";
    case autodim::BEDTIME: return "Dimming for bed";
    case autodim::NIGHT: return "Night";
    default: return "Waiting for the time";
  }
}

struct AutoDimMode : Screen {
  ListWheel wheel;
  String selId = "on";
  uint32_t lastRefresh = 0;

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    bool on = autodim::enabled();
    v.push_back({"on", "Auto brightness", on ? "On" : "Off",
                 !on ? String("Follows the sun and your bedtime")
                 : autodim::phase() == autodim::PAUSED ? String("Waits until the clock is set")
                 : phaseText(autodim::phase()) + ", " + (int)lroundf(autodim::current()) + "% now",
                 on ? ListItem::Ok : ListItem::Normal});
    v.push_back({"wake", "Wake time", minText(autodim::wakeMinutes()), "Full an hour later"});
    v.push_back({"bed", "Bedtime", minText(autodim::bedMinutes()), "Dims the hour before"});
    v.push_back({"max", "Day max", String(autodim::dayMax()) + "%", ""});
    v.push_back({"min", "Night min", String(autodim::nightMin()) + "%", "Dimmest at night"});
    return v;
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
    wheel.onPick = [this](int) { open(); };
    wheel.onChange = [this]() { if (const ListItem *it = wheel.selectedItem()) selId = it->id; };
    ui::top(root, "Auto brightness");
  }

  void open() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "on") {
      autodim::setEnabled(!autodim::enabled());
      ui::toast(autodim::enabled() ? "Auto brightness on" : "Auto brightness off", 1);
      wheel.setItems(items());
    } else if (it->id == "wake") {
      halfHourFlow("Wake time", autodim::wakeMinutes(), 8, 22,
                   [](int m) { return "Full brightness by " + minText(m + 60); }, [](int m) {
        autodim::setWakeMinutes(m);
        ui::toast((String("Wake ") + minText(m)).c_str(), 1);
      });
    } else if (it->id == "bed") {
      halfHourFlow("Bedtime", autodim::bedMinutes(), 40, 52,
                   [](int m) { return "Starts dimming at " + minText(m - 60); }, [](int m) {
        autodim::setBedMinutes(m);
        ui::toast((String("Bedtime ") + minText(m)).c_str(), 1);
      });
    } else if (it->id == "max") {
      app::push(new BrightnessMode(Dial::DayMax));
    } else if (it->id == "min") {
      app::push(new BrightnessMode(Dial::NightMin));
    }
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { open(); }
  void tick() override {
    wheel.tick();
    if (millis() - lastRefresh > 1000) {  // the live level and phase
      lastRefresh = millis();
      wheel.setItems(items());
    }
  }
};

// ---------- Sound ----------
// Volume for every sound, and a separate switch for the quiet knob and typing clicks. The
// buzzer is an optional add-on (docs/hardware.md); without one, nothing plays either way.
void volumeFlow() {
  std::vector<String> opts;
  for (int v = sounds::VOL_OFF; v <= sounds::VOL_HIGH; v++) opts.push_back(sounds::volumeName((sounds::Volume)v));
  app::push(new PickerMode("Volume", "Sound volume", opts, sounds::volume(), [](int i) {
    sounds::setVolume((sounds::Volume)i);
    sounds::preview();
    ui::toast(i ? (String("Volume ") + sounds::volumeName((sounds::Volume)i)).c_str() : "Sound off", 1);
  }));
}

// Settings > Sound > Sounds: every event with the sound it plays. Opening one lists its
// suggestions, then each category of the library (Melodies, Appliances, ...), which opens to
// its sounds. Turning through sounds plays each one; press picks it.
struct EventSoundMode : Screen {
  sounds::Event ev;
  ListWheel wheel;
  int sel = 0;
  bool heard = false;  // the highlighted sound has played once on entry

  // Opens on the sound in use: its suggestion row, or the category it's in.
  explicit EventSoundMode(sounds::Event e) : ev(e) {
    std::vector<ListItem> its = items();
    for (int i = 0; i < (int)its.size(); i++)
      if (its[i].tone == ListItem::Ok) { sel = i; break; }
  }

  std::vector<ListItem> items() {
    const sounds::EventInfo &info = sounds::EVENTS[ev];
    int cur = sounds::soundFor(ev);
    std::vector<ListItem> v;
    bool curShown = false;
    if (sounds::hasCustom(ev)) {  // sent from the sound lab
      ListItem it{"u", sounds::customName(ev), "", "From the sound lab"};
      if (cur == sounds::CUSTOM) { it.badge = LV_SYMBOL_OK; it.tone = ListItem::Ok; curShown = true; }
      v.push_back(it);
    }
    for (int k = 0; k < info.nSuggest; k++) {
      int i = info.suggest[k];
      ListItem it{String("s") + i, sounds::soundName(i), "", i == info.def ? "Default" : "Suggested"};
      if (i == cur) { it.badge = LV_SYMBOL_OK; it.tone = ListItem::Ok; curShown = true; }
      v.push_back(it);
    }
    for (int c = 0; c < sounds::CATEGORY_COUNT; c++) {
      int n = 0;
      for (int i = 0; i < sounds::SOUND_COUNT; i++) n += sounds::SOUNDS[i].cat == c;
      bool here = !curShown && cur >= 0 && sounds::SOUNDS[cur].cat == c;
      v.push_back({String("c") + c, sounds::CATEGORIES[c], String(n) + " " LV_SYMBOL_RIGHT,
                   here ? String("In use: ") + sounds::soundName(cur) : String("")});
      if (here) v.back().tone = ListItem::Ok;
    }
    return v;
  }

  void build(lv_obj_t *root) override {
    wheel = ListWheel();
    wheel.build(root);
    wheel.setItems(items());
    wheel.jump(constrain(sel, 0, (int)items().size() - 1));
    wheel.onPick = [this](int) { open(); };
    wheel.onChange = [this]() {
      int was = sel;
      sel = wheel.selected();
      const ListItem *it = wheel.selectedItem();
      if (sel != was && it) hear(*it);
    };
    ui::top(root, sounds::EVENTS[ev].label);
    if (!heard) {  // hear the highlighted sound right away, not only after turning away and back
      heard = true;
      if (const ListItem *it = wheel.selectedItem()) hear(*it);
    }
  }

  void hear(const ListItem &it) {
    if (it.id == "u") sounds::playSound(sounds::CUSTOM, ev);
    else if (it.id[0] == 's') sounds::playSound(it.id.substring(1).toInt());
  }

  void open() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id[0] == 's' || it->id == "u") {
      int i = it->id == "u" ? sounds::CUSTOM : it->id.substring(1).toInt();
      sounds::setSound(ev, i);
      sounds::playSound(i, ev);
      ui::toast((String(sounds::EVENTS[ev].label) + ": " + sounds::pickName(ev)).c_str(), 1);
      app::pop();
      return;
    }
    int c = it->id.substring(1).toInt();
    std::vector<int> idx;
    std::vector<String> names;
    int cur = -1;
    for (int i = 0; i < sounds::SOUND_COUNT; i++)
      if (sounds::SOUNDS[i].cat == c) {
        if (i == sounds::soundFor(ev)) cur = idx.size();
        idx.push_back(i);
        names.push_back(String(sounds::SOUNDS[i].name) + (i == sounds::EVENTS[ev].def ? " (default)" : ""));
      }
    sounds::Event e = ev;
    auto *p = new PickerMode(sounds::CATEGORIES[c], "Sound", names, cur, [e, idx](int k) {
      sounds::setSound(e, idx[k]);
      sounds::playSound(idx[k]);
      ui::toast((String(sounds::EVENTS[e].label) + ": " + sounds::soundName(idx[k])).c_str(), 1);
      app::pop();  // back to the Sounds list
    });
    p->onHover = [idx](int k) { sounds::playSound(idx[k]); };
    app::push(p);
    sounds::playSound(idx[cur >= 0 ? cur : 0]);  // the highlighted one plays on entry
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { open(); }
  void tick() override { wheel.tick(); }
};

struct SoundsMode : Screen {
  ListWheel wheel;
  int sel = 0;

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    for (int e = 0; e < sounds::EVENT_COUNT; e++) {
      int k = sounds::soundFor((sounds::Event)e);
      v.push_back({String(e), sounds::EVENTS[e].label, sounds::pickName((sounds::Event)e),
                   k == sounds::EVENTS[e].def ? "Default" : k == sounds::CUSTOM ? "From the sound lab" : "Changed"});
    }
    if (sounds::changedCount()) v.push_back({"reset", "Reset to defaults", "", String(sounds::changedCount()) + " changed"});
    return v;
  }

  void build(lv_obj_t *root) override {
    wheel = ListWheel();
    wheel.build(root);
    wheel.setItems(items());
    wheel.jump(constrain(sel, 0, (int)items().size() - 1));
    wheel.onPick = [this](int) { open(); };
    wheel.onChange = [this]() { sel = wheel.selected(); };
    ui::top(root, "Sounds");
  }

  void open() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "reset") {
      sounds::resetChoices();
      ui::toast("All sounds back to defaults", 1);
      sel = 0;
      app::refresh();
      return;
    }
    if (sounds::volume() == sounds::VOL_OFF) ui::toast("Volume is off: turn it up to hear them", -1);
    app::push(new EventSoundMode((sounds::Event)it->id.toInt()));
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { open(); }
  void tick() override { wheel.tick(); }
};

struct SoundMode : Screen {
  ListWheel wheel;
  String selId = "vol";

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    sounds::Volume vol = sounds::volume();
    v.push_back({"vol", "Volume", sounds::volumeName(vol), vol ? "Timers, reminders, games" : "Everything is silent"});
    v.push_back({"clicks", "Knob clicks", sounds::clicks() ? "On" : "Off", "Soft ticks as you turn, press and type",
                 sounds::clicks() && vol ? ListItem::Ok : ListItem::Normal});
    int n = sounds::changedCount();
    v.push_back({"sounds", "Sounds", n ? String(n) + " changed" : String("Defaults"), "Pick the sound for each event"});
    return v;
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
    wheel.onPick = [this](int) { open(); };
    wheel.onChange = [this]() { if (const ListItem *it = wheel.selectedItem()) selId = it->id; };
    ui::top(root, "Sound");
  }

  void open() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "vol") {
      volumeFlow();
    } else if (it->id == "clicks") {
      sounds::setClicks(!sounds::clicks());
      ui::toast(sounds::clicks() ? "Knob clicks on" : "Knob clicks off", 1);
      wheel.setItems(items());
    } else if (it->id == "sounds") {
      app::push(new SoundsMode());
    }
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { open(); }
  void tick() override { wheel.tick(); }
};

// ---------- Panels: show, hide and reorder ----------
// Picked A2 in panels-mockups.html (2026-11-10). Both screens are a flat ListWheel: every
// row the same size and brightness, drag and flick to scroll like the other lists.
// Panels: press or tap a row flips its switch (Home and Settings can't be hidden). The Reorder
// chip sits at the bottom, always visible (instead of the "N shown" line); the knob
// reaches it by turning past the last panel.
// Reorder: tap a row's up/down arrows to move it one place; the knob picks the middle row up
// (press), moves it (turn) and drops it (press). Done or hold goes back. As in every list, the
// outline only shows for the knob (ListWheel); a picked-up row always shows. Tapping a row
// itself does nothing here: the arrows move it.

void savePanelOrder(const std::vector<Panel *> &order) {
  std::vector<String> ids;
  for (Panel *p : order) ids.push_back(p->id);
  store::setPanelOrder(ids);
}

// The row's position number, left of the title (the wheel leaves leftInset for it).
void panelNumber(lv_obj_t *area, int i) {
  lv_obj_t *pos = ui::label(area, String(i + 1).c_str(), &lv_font_montserrat_16, theme::inkDim());
  lv_obj_set_width(pos, 26);
  lv_obj_set_style_text_align(pos, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(pos, LV_ALIGN_LEFT_MID, 8, 1);
}

constexpr int PANEL_INSET = 30;

struct ReorderMode : Screen {
  ListWheel wheel;
  bool lifted = false;
  void showOutline() { wheel.setLifted(lifted); }

  std::vector<ListItem> items() {
    std::vector<Panel *> order = app::orderedPanels();
    std::vector<ListItem> v;
    int n = order.size();
    for (int k = 0; k < n; k++) {
      Panel *p = order[k];
      bool hid = !app::locked(p) && store::hidden(p->id);
      // badge: which arrows work, so a row redraws when it reaches an end
      String ends = String(k == 0 ? "t" : "") + (k == n - 1 ? "b" : "");
      v.push_back({p->id, p->name, ends, "", hid ? ListItem::Past : ListItem::Normal});
    }
    return v;
  }

  static void arrowCb(lv_event_t *e) {
    auto *self = (ReorderMode *)lv_event_get_user_data(e);
    if (self->wheel.touchMoved()) return;  // the touch was a drag that started on the arrow
    intptr_t v = (intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
    int k = (int)(v >> 1), dir = (v & 1) ? 1 : -1;
    self->lifted = false;
    self->move(k, k + dir);
  }

  static lv_obj_t *arrow(lv_obj_t *area, int k, int dir, bool enabled, ReorderMode *self) {
    lv_obj_t *b = lv_obj_create(area);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 38, 38);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, 20, 0);
    lv_obj_set_style_bg_color(b, theme::accent(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = ui::label(b, dir < 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, &lv_font_montserrat_16,
                            enabled ? theme::ink() : theme::inkFaint());
    lv_obj_center(l);
    if (enabled) {
      lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);  // a drag starting here still scrolls the list
      lv_obj_set_ext_click_area(b, 5);
      lv_obj_set_user_data(b, (void *)(intptr_t)((k << 1) | (dir > 0 ? 1 : 0)));
      lv_obj_add_event_cb(b, arrowCb, LV_EVENT_CLICKED, self);
    } else {
      lv_obj_set_style_bg_opa(b, 6, 0);
    }
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, dir < 0 ? -52 : -8, 0);
    return b;
  }

  void build(lv_obj_t *root) override {
    wheel = ListWheel();
    wheel.flat = true;
    wheel.leftInset = PANEL_INSET;
    wheel.decorate = [this](lv_obj_t *area, int i, const ListItem &it) {
      panelNumber(area, i);
      arrow(area, i, -1, it.badge.indexOf('t') < 0, this);
      arrow(area, i, 1, it.badge.indexOf('b') < 0, this);
    };
    wheel.build(root);
    std::vector<ListItem> its = items();
    wheel.setItems(its);
    wheel.jump(sel);
    showOutline();
    ui::top(root, "Reorder");
    lv_obj_t *done = ui::chip(root, "Done", ui::Tone::Primary, [](lv_event_t *) { app::pop(); }, this);
    lv_obj_align(done, LV_ALIGN_TOP_MID, 0, 416);  // below the cut-off row (no status line: no room)
  }

  int sel = 0;

  void move(int from, int to) {
    std::vector<Panel *> order = app::orderedPanels();
    if (to < 0 || to >= (int)order.size() || from == to) return;
    std::swap(order[from], order[to]);
    savePanelOrder(order);
    sel = to;
    wheel.setItems(items());
    wheel.jump(to);
    showOutline();
  }

  void rotate(int dir) override {
    if (lifted) move(wheel.selected(), wheel.selected() + dir);
    else { wheel.rotate(dir); sel = wheel.selected(); }
  }

  void press() override {
    lifted = !lifted;
    sel = wheel.selected();
    showOutline();
    if (lifted) ui::toast("Turn to move, press to drop");
    if (!lifted) {
      String msg = String(app::orderedPanels()[sel]->name) + " is now #" + (sel + 1);
      ui::toast(msg.c_str(), 1);
    }
  }

  void tick() override { wheel.tick(); }
};

struct PanelsMode : Screen {
  ListWheel wheel;
  lv_obj_t *chip = nullptr;
  bool onChip = false;  // the knob is on the Reorder chip (past the last panel)
  String selId;

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    for (Panel *p : app::orderedPanels()) {
      bool locked = app::locked(p);
      bool hid = !locked && store::hidden(p->id);
      bool noWifi = !strcmp(p->id, "weather") && !store::wifi() && !hid;
      // badge: the switch's state, so the row redraws when it flips
      String st = locked ? "lock" : hid ? "off" : noWifi ? "wifi" : "on";
      v.push_back({p->id, p->name, st, "", hid ? ListItem::Past : ListItem::Normal});
    }
    return v;
  }

  // An on/off switch drawn from two rounded boxes (not touchable itself: the row is).
  static void drawSwitch(lv_obj_t *area, bool on, bool locked) {
    lv_obj_t *sw = lv_obj_create(area);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 48, 28);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sw, on ? theme::ok() : lv_color_hex(0x2A2F3A), 0);
    lv_obj_set_style_bg_opa(sw, locked ? LV_OPA_40 : LV_OPA_COVER, 0);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_t *knob = lv_obj_create(sw);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, 22, 22);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, on ? lv_color_white() : lv_color_hex(0xCFD5DF), 0);
    lv_obj_set_style_bg_opa(knob, locked ? LV_OPA_50 : LV_OPA_COVER, 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -3 : 3, 0);
  }

  void build(lv_obj_t *root) override {
    wheel = ListWheel();
    wheel.flat = true;
    wheel.leftInset = PANEL_INSET;
    wheel.decorate = [](lv_obj_t *area, int i, const ListItem &it) {
      panelNumber(area, i);
      drawSwitch(area, it.badge != "off", it.badge == "lock");
      if (it.badge == "wifi") {
        lv_obj_t *tag = ui::label(area, "No Wi-Fi", &lv_font_montserrat_14, theme::inkDim());
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -72, 0);
      }
    };
    wheel.onPick = [this](int) {
      if (onChip) { onChip = false; ui::select(chip, false); wheel.setHighlight(true); }
      flip();
    };
    wheel.onChange = [this]() { if (const ListItem *it = wheel.selectedItem()) selId = it->id; };
    wheel.build(root);
    std::vector<ListItem> its = items();
    int start = 0;
    for (int i = 0; i < (int)its.size(); i++)
      if (its[i].id == selId) start = i;
    wheel.setItems(its);
    wheel.jump(start);
    ui::top(root, "Panels");
    chip = ui::chip(root, "Reorder", ui::Tone::Normal, [](lv_event_t *e) {
      app::push(new ReorderMode());
    }, this);
    lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 416);  // below the cut-off row
    ui::select(chip, onChip);
    wheel.setHighlight(!onChip);
  }

  void flip() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    Panel *p = app::orderedPanels()[wheel.selected()];
    if (app::locked(p)) {
      ui::toast((String(p->name) + " is always shown").c_str());
      return;
    }
    bool hid = store::hidden(p->id);
    store::setHidden(p->id, !hid);
    ui::toast((String(p->name) + (hid ? " shown" : " hidden")).c_str(), hid ? 1 : 0);
    wheel.setItems(items());
  }

  void rotate(int dir) override {
    if (!wheel.knobShown() && !onChip) { wheel.rotate(dir); return; }  // first turn: show the outline
    int last = (int)app::orderedPanels().size() - 1;
    if (onChip) {
      if (dir < 0) { onChip = false; ui::select(chip, false); wheel.setHighlight(true); }
      return;
    }
    if (dir > 0 && wheel.selected() >= last) {  // past the last panel: the Reorder chip
      onChip = true;
      ui::select(chip, true);
      wheel.setHighlight(false);
      return;
    }
    wheel.rotate(dir);
  }

  void press() override {
    if (onChip) app::push(new ReorderMode());
    else flip();
  }

  void tick() override { wheel.tick(); }
};

// ---------- Date & time ----------
void manualTimeFlow() {
  struct tm now = clockx::local();
  int y = now.tm_year + 1900;
  std::vector<Field> date = {
    {"Month", now.tm_mon + 1, 1, 12, nullptr, [](int v) { return String(MONTHS[v - 1]); }},
    {"Day", now.tm_mday, 1, 31, [](const std::vector<Field> &f) { return daysIn(f[2].value, f[0].value); }, nullptr, 5},
    {"Year", y, y - 1, y + 5, nullptr, nullptr},
  };
  app::push(new FieldsMode("Date", date,
    [](const std::vector<Field> &f) {
      struct tm t = {};
      t.tm_year = f[2].value - 1900; t.tm_mon = f[0].value - 1; t.tm_mday = f[1].value; t.tm_hour = 12;
      mktime(&t);
      const char *wd = daynum::dayLong(t.tm_wday), *mo = daynum::monthLong(f[0].value);
      return tz::dayFirst() ? String(wd) + " " + f[1].value + " " + mo + " " + f[2].value
                            : String(wd) + ", " + mo + " " + f[1].value + ", " + f[2].value;
    },
    [now](const std::vector<Field> &d) {
      int mo = d[0].value, day = d[1].value, yr = d[2].value;
      int h = now.tm_hour;
      auto two = [](int v) { char b[4]; snprintf(b, 4, "%02d", v); return String(b); };
      std::vector<Field> time;
      if (tz::h24()) {
        time = {{"Hour", h, 0, 23, nullptr, two}, {"Min", now.tm_min, 0, 59, nullptr, two, 5}};
      } else {
        time = {{"Hour", h % 12 == 0 ? 12 : h % 12, 1, 12, nullptr, nullptr},
                {"Min", now.tm_min, 0, 59, nullptr, two, 5},
                {"", h >= 12 ? 1 : 0, 0, 1, nullptr, [](int v) { return String(v ? "PM" : "AM"); }}};
      }
      app::push(new FieldsMode("Time", time,
        [](const std::vector<Field> &f) {
          int hour = f.size() == 2 ? f[0].value : f[0].value % 12 + (f[2].value ? 12 : 0);
          return tz::timeText(hour, f[1].value);
        },
        [yr, mo, day](const std::vector<Field> &t) {
          int hour = t.size() == 2 ? t[0].value : t[0].value % 12 + (t[2].value ? 12 : 0);
          net::setAutoTime(false);  // a hand-set clock shouldn't be overwritten by network time
          clockx::setLocal(yr, mo, day, hour, t[1].value);
          ui::toast("Clock set", 1);
        }));
    },
    [](std::vector<Field> &f) { int dim = daysIn(f[2].value, f[0].value); if (f[1].value > dim) f[1].value = dim; }));
}

void timeFlow() {
  if (!net::configured()) { manualTimeFlow(); return; }
  bool autoOn = net::autoTime();
  app::push(new PickerMode("Date & time", "", {"Automatic (Wi-Fi)", "Set manually"}, autoOn ? 0 : 1, [](int i) {
    if (i == 0) { net::setAutoTime(true); ui::toast("Using Wi-Fi time", 1); }
    else manualTimeFlow();
  }));
}

// The device's own time zone, picked by city (the same list as World). A weather location
// without a preset of its own (Bangkok) comes first: its time, from the forecast.
void zoneFlow() {
  std::vector<String> opts;
  time_t now = time(nullptr);
  bool loc = tz::hasLocationZone();
  if (loc) {
    int32_t lt = (int32_t)((now + tz::locationOffset()) % 86400);
    if (lt < 0) lt += 86400;
    opts.push_back(tz::locationZoneName() + " \xE2\x80\xA2 " + tz::timeText(lt / 3600, lt / 60 % 60));
  }
  for (int i = 0; i < tz::CITY_COUNT; i++) {
    tz::Local l = tz::at(tz::CITIES[i].posix, now);
    opts.push_back(String(tz::CITIES[i].name) + " \xE2\x80\xA2 " + tz::timeText(l.hour, l.min));
  }
  int cur = tz::deviceCity() < 0 ? 0 : tz::deviceCity() + (loc ? 1 : 0);
  app::push(new PickerMode("Time zone", "Same time as", opts, cur, [loc](int i) {
    if (loc && i == 0) {
      tz::useLocationZone();
      ui::toast((String("Time zone: ") + tz::locationZoneName()).c_str(), 1);
      return;
    }
    int c = i - (loc ? 1 : 0);
    tz::setDeviceCity(c);
    ui::toast((String("Time zone: ") + tz::CITIES[c].name).c_str(), 1);
  }));
}

// 12- or 24-hour clock: Auto follows the location's country.
void clockFlow() {
  std::vector<String> opts = {String("Auto (") + (tz::autoH24() ? "24-hour" : "12-hour") + " here)", "12-hour", "24-hour"};
  app::push(new PickerMode("Time format", "Clock", opts, (int)tz::clockPref(), [](int i) {
    tz::setClockPref((tz::ClockPref)i);
    ui::toast(tz::h24() ? "24-hour clock" : "12-hour clock", 1);
  }));
}

void idleFlow() {
  static const int VALS[] = {0, 1, 2, 5, 10};
  std::vector<String> opts;
  int cur = 2;
  for (int i = 0; i < 5; i++) {
    opts.push_back(VALS[i] ? String("After ") + VALS[i] + " min" : String("Never"));
    if (VALS[i] == app::idleMinutes()) cur = i;
  }
  app::push(new PickerMode("Return to Home", "When idle", opts, cur, [](int i) {
    app::setIdleMinutes(VALS[i]);
    ui::toast(VALS[i] ? (String("Home after ") + VALS[i] + " min").c_str() : "Stays where you leave it", 1);
  }));
}

// ---------- Weather (once Wi-Fi is set up; the first setup is on the phone) ----------
// Location: type a city on the ring, pick from the matches (works for any country; the
// geocoder knows place names everywhere, while postal codes are patchy outside the US).
// Units: Auto (by the location's country), or fixed.

void findCity();

// Waits for a search, then lists the matches. No match or no connection: a short screen with
// Search again / Back.
struct SearchMode : Screen {
  String q;
  ListWheel wheel;
  bool listed = false;
  weather::Search seen = weather::Search::Busy;
  std::vector<weather::Place> res;
  String setting;  // a picked place whose forecast is being fetched before it's saved
  explicit SearchMode(String q) : q(q) {}

  void build(lv_obj_t *root) override {
    listed = false;
    if (setting.length()) {
      ui::top(root, "Weather");
      lv_obj_t *col = ui::column(root, 10);
      ui::label(col, "Getting the weather for", &lv_font_montserrat_20, theme::inkDim());
      ui::label(col, setting.c_str(), &lv_font_montserrat_34, theme::ink());
      return;
    }
    if (seen == weather::Search::Done && !res.empty()) {
      wheel = ListWheel();
      wheel.build(root);
      std::vector<ListItem> items;
      for (int i = 0; i < (int)res.size(); i++) {
        const weather::Place &p = res[i];
        String where = p.region.length() && p.region != p.name ? p.region + ", " + p.country : p.country;
        items.push_back({String(i), p.name, p.cc, where});
      }
      wheel.setItems(items);
      wheel.jump(0);
      wheel.onPick = [this](int i) { choose(i); };
      ui::top(root, "Pick a place");
      listed = true;
      return;
    }
    ui::top(root, "Weather");
    lv_obj_t *col = ui::column(root, 10);
    ui::label(col, "Searching for", &lv_font_montserrat_20, theme::inkDim());
    ui::label(col, q.c_str(), &lv_font_montserrat_34, theme::ink());
  }

  void problem(const String &title, const String &sub) {
    sounds::play(sounds::ERROR);
    auto *d = new DetailMode();
    d->topText = "Weather";
    d->body = [title, sub]() {
      return std::vector<Line>{{title, &lv_font_montserrat_28, theme::ink()}, {sub, &lv_font_montserrat_20, theme::inkDim()}};
    };
    d->controls = [d]() {
      return std::vector<Control>{
        {"Search again", ui::Tone::Primary, [d]() { app::drop(d); findCity(); }},
        {"Back", ui::Tone::Normal, [d]() { app::drop(d); }},
      };
    };
    app::drop(this);
    app::push(d);
  }

  void tick() override {
    if (setting.length()) {
      weather::Choose c = weather::chooseState();
      if (c == weather::Choose::Done) {
        ui::toast((String("Weather: ") + setting).c_str(), 1);
        app::drop(this);
      } else if (c == weather::Choose::Failed) {
        // The previous location stays; nothing was saved.
        problem("Couldn't set " + setting, "Check the Wi-Fi connection and try again. The weather stays on " +
                (weather::locationLabel().length() ? weather::locationLabel() : String("no location")) + ".");
      }
      return;
    }
    if (listed) wheel.tick();
    weather::Search st = weather::searchState();
    if (st == seen) return;
    seen = st;
    if (st == weather::Search::Done) {
      res = weather::searchResults();
      if (res.empty()) { problem("No places called " + q, "Check the spelling, or try a bigger town nearby"); return; }
      app::refresh();
    } else if (st == weather::Search::Failed) {
      problem("Couldn't search", "Check the Wi-Fi connection");
    }
  }

  void choose(int i) {
    if (i < 0 || i >= (int)res.size()) return;
    weather::choose(res[i]);
    setting = res[i].name;
    app::refresh();
  }

  void rotate(int dir) override { if (listed && !setting.length()) wheel.rotate(dir); }
  void press() override { if (listed && !setting.length()) choose(wheel.selected()); }
};

void findCity() {
  app::push(new TextMode("Find a city", "", [](const String &q) {
    weather::search(q);
    app::push(new SearchMode(q));
  }, 24));
}

const char *const DEG_F = "\xC2\xB0" "F";
const char *const DEG_C = "\xC2\xB0" "C";

void unitsFlow() {
  String autoLabel = String("Auto (") + (weather::autoCelsius() ? DEG_C : DEG_F) + " here)";
  std::vector<String> opts = {autoLabel, String(DEG_F) + " and mph", String(DEG_C) + " and km/h"};
  app::push(new PickerMode("Units", "Units", opts, (int)weather::units(), [](int i) {
    weather::setUnits((weather::Units)i);
    ui::toast(i == 0 ? "Units follow the location" : i == 1 ? "Fahrenheit" : "Celsius", 1);
  }));
}

struct WeatherMode : Screen {
  ListWheel wheel;
  String selId = "loc";
  uint32_t lastRefresh = 0;

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    String lab = weather::locationLabel();
    int c = lab.indexOf(',');
    String name = c > 0 ? lab.substring(0, c) : lab;
    String rest = c > 0 ? lab.substring(c + 1) : String("");
    rest.trim();
    bool none = !lab.length();
    bool lost = !none && weather::state() == weather::State::NotFound;
    v.push_back({"loc", "Location", none ? String("Not set") : name,
                 lost ? String("Not found; search for it here") : rest.length() ? rest : String("Search for a city"),
                 lost ? ListItem::Past : ListItem::Normal});
    weather::Units u = weather::units();
    bool ac = weather::autoCelsius();
    v.push_back({"units", "Units", u == weather::UNITS_AUTO ? String("Auto") : String(u == weather::UNITS_F ? DEG_F : DEG_C),
                 u == weather::UNITS_AUTO ? String(ac ? DEG_C : DEG_F) + " and " + (ac ? "km/h" : "mph") + " here"
                                          : String(u == weather::UNITS_F ? "Wind in mph" : "Wind in km/h")});
    return v;
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
    wheel.onPick = [this](int) { open(); };
    wheel.onChange = [this]() { if (const ListItem *it = wheel.selectedItem()) selId = it->id; };
    ui::top(root, "Weather");
  }

  void open() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "loc") findCity();
    else if (it->id == "units") unitsFlow();
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { open(); }
  void tick() override {
    wheel.tick();
    if (millis() - lastRefresh > 1000) {  // the location's state, Auto's meaning
      lastRefresh = millis();
      wheel.setItems(items());
    }
  }
};

String wifiStatus() {
  switch (net::status()) {
    case net::Status::Connected: return "Connected";
    case net::Status::Connecting: return "Connecting...";
    case net::Status::Failed: return "Can't connect";
    default: return "Set up from your phone";
  }
}

void reminderPicker(store::Reminder r) {
  static const int VALS[] = {0, 15, 20, 30, 45, 60, 90};
  std::vector<String> opts;
  int cur = 0;
  for (int i = 0; i < 7; i++) {
    opts.push_back(VALS[i] ? String("Every ") + VALS[i] + " min" : String("Off"));
    if (VALS[i] == store::reminderMinutes(r)) cur = i;
  }
  app::push(new PickerMode(reminders::info(r).name, "How often?", opts, cur, [r](int i) {
    store::setReminderMinutes(r, VALS[i]);
    reminders::reset(r);
    ui::toast(VALS[i] ? (String("Every ") + VALS[i] + " min").c_str() : "Reminder off", 1);
  }));
}

// Reminder hours: presets, or Custom (from, to, days).
void hoursFlow() {
  using reminders::Hours;
  static const Hours PRESETS[] = {
    {false, 8, 18, true}, {false, 9, 17, true}, {false, 8, 20, false}, {true, 8, 18, true},
  };
  Hours cur = reminders::hours();
  std::vector<String> opts;
  int curIdx = 4;  // Custom
  for (int i = 0; i < 4; i++) {
    const Hours &p = PRESETS[i];
    opts.push_back(p.allDay ? String("Any time") : reminders::daysText(p) + ", " + reminders::hoursText(p));
    bool same = p.allDay ? cur.allDay : (!cur.allDay && p.from == cur.from && p.to == cur.to && p.weekdays == cur.weekdays);
    if (same) curIdx = i;
  }
  opts.push_back(curIdx == 4 ? String("Custom: ") + reminders::daysText(cur) + ", " + reminders::hoursText(cur) : String("Custom..."));
  app::push(new PickerMode("Reminder hours", "When reminders may fire", opts, curIdx, [](int i) {
    if (i < 4) {
      reminders::setHours(PRESETS[i]);
      Hours h = PRESETS[i];
      ui::toast(h.allDay ? "Reminders any time" : (reminders::daysText(h) + ", " + reminders::hoursText(h)).c_str(), 1);
      return;
    }
    Hours c = reminders::hours();
    if (c.allDay) c = {false, 8, 18, true};
    auto hourFmt = [](int h) { return tz::hourText(h); };
    std::vector<Field> f = {
      {"From", c.from, 0, 23, nullptr, hourFmt},
      {"To", c.to, 0, 23, nullptr, hourFmt},
      {"Days", c.weekdays ? 1 : 0, 0, 1, nullptr, [](int v) { return String(v ? "Weekdays" : "Every day"); }},
    };
    app::push(new FieldsMode("Reminder hours", f,
      [](const std::vector<Field> &v) {
        Hours h{false, v[0].value, v[1].value, v[2].value != 0};
        return reminders::daysText(h) + ", " + reminders::hoursText(h);
      },
      [](const std::vector<Field> &v) {
        Hours h{false, v[0].value, v[1].value, v[2].value != 0};
        reminders::setHours(h);
        ui::toast((reminders::daysText(h) + ", " + reminders::hoursText(h)).c_str(), 1);
      }));
  }));
}

// ---------- the Settings list ----------
struct SettingsPanel : Panel {
  ListWheel wheel;
  lv_obj_t *counter = nullptr;
  uint32_t lastRefresh = 0;
  int shownMinute = -1;
  String selId = "wifi";

  SettingsPanel() : Panel("settings", "Settings", &icon_settings) {}

  std::vector<ListItem> items() {
    struct tm now = clockx::local();
    char when[32];
    if (tz::dayFirst()) snprintf(when, sizeof(when), "%d %s, %s", now.tm_mday, MONTHS[now.tm_mon], timeText(now).c_str());
    else snprintf(when, sizeof(when), "%s %d, %s", MONTHS[now.tm_mon], now.tm_mday, timeText(now).c_str());
    std::vector<ListItem> v;
    bool wifi = net::configured();
    bool connected = net::status() == net::Status::Connected;
    v.push_back({"wifi", "Wi-Fi", wifi ? net::ssid() : String("Off"), wifiStatus(), connected ? ListItem::Ok : ListItem::Normal});
    // Until the clock has been set it runs from the firmware's build time: say so, not the date.
    String timeSub = clockx::trusted() ? String(when)
                   : wifi && net::autoTime() ? String("Waiting for network time") : String("Not set yet; set it here");
    v.push_back({"time", "Date & time", wifi && net::autoTime() ? "Automatic" : "Manual", timeSub,
                 clockx::trusted() ? ListItem::Normal : ListItem::Past});
    {
      int c = tz::deviceCity();
      int32_t off = tz::offset(tz::devicePosix(), time(nullptr)) / 60;
      char utc[16];
      if (off % 60) snprintf(utc, sizeof(utc), "UTC%+ld:%02ld", (long)(off / 60), (long)abs(off % 60));
      else snprintf(utc, sizeof(utc), off ? "UTC%+ld" : "UTC", (long)(off / 60));
      v.push_back({"tz", "Time zone", c < 0 ? tz::locationZoneName() : String(tz::CITIES[c].name), utc});
    }
    {
      tz::ClockPref p = tz::clockPref();
      v.push_back({"clock", "Time format", tz::h24() ? "24-hour" : "12-hour",
                   p == tz::CLOCK_AUTO ? "Auto, from the location" : "Set by hand"});
    }
    v.push_back({"bright", "Brightness", String((int)lroundf(autodim::current())) + "%",
                 autodim::enabled() ? (autodim::overriding() ? resumeText() : "Auto brightness on") : "Screen backlight"});
    v.push_back({"autodim", "Auto brightness", autodim::enabled() ? "On" : "Off",
                 autodim::enabled() ? "Wake " + minText(autodim::wakeMinutes()) + ", bed " + minText(autodim::bedMinutes())
                                    : String("Brightness stays where you set it")});
    v.push_back({"sound", "Sound", sounds::volumeName(sounds::volume()),
                 !sounds::volume() ? String("Silent") : sounds::clicks() ? String("Knob clicks on") : String("Knob clicks off")});
    for (int i = 0; i < store::REMINDER_COUNT; i++) {
      auto r = (store::Reminder)i;
      uint8_t m = store::reminderMinutes(r);
      v.push_back({String("rem") + i, reminders::info(r).name, m ? String(m) + " min" : String("Off"), reminders::info(r).sub});
    }
    {
      reminders::Hours h = reminders::hours();
      v.push_back({"hours", "Reminder hours", reminders::hoursText(h),
                   h.allDay ? String("Reminders fire day and night") : reminders::daysText(h) + (reminders::activeNow() ? ", on now" : ", off now")});
    }
    {
      String lab = weather::locationLabel();
      int c = lab.indexOf(',');
      v.push_back({"weather", "Weather", !wifi ? String("Needs Wi-Fi") : !lab.length() ? String("No location") : c > 0 ? lab.substring(0, c) : lab,
                   wifi ? "Location and units" : "Set up from your phone"});
    }
    {
      int m = app::idleMinutes();
      v.push_back({"idle", "Return to Home", m ? String("After ") + m + " min" : String("Off"),
                   "When idle, unless something's running"});
    }
    v.push_back({"panels", "Panels", String(app::shownPanels().size()) + " shown", "Show, hide and reorder"});
    return v;
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
    shownMinute = clockx::local().tm_min;
  }

  void remember() {
    if (const ListItem *it = wheel.selectedItem()) selId = it->id;
    lv_label_set_text_fmt(counter, "%d / %d", wheel.selected() + 1, (int)items().size());
  }

  void pick() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    String id = it->id;
    Serial.printf("[settings] open %s\n", id.c_str());
    if (id == "weather" && net::configured()) app::push(new WeatherMode());
    else if (id == "wifi" || id == "weather") app::push(new SetupMode());
    else if (id == "time") timeFlow();
    else if (id == "tz") zoneFlow();
    else if (id == "clock") clockFlow();
    else if (id == "idle") idleFlow();
    else if (id == "bright") app::push(new BrightnessMode());
    else if (id == "autodim") app::push(new AutoDimMode());
    else if (id == "sound") app::push(new SoundMode());
    else if (id == "panels") app::push(new PanelsMode());
    else if (id == "hours") hoursFlow();
    else if (id.startsWith("rem")) reminderPicker((store::Reminder)(id.substring(3).toInt()));
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { pick(); }
  void tick() override {
    wheel.tick();
    int m = clockx::local().tm_min;
    if (m != shownMinute || millis() - lastRefresh > 1000) {  // clock row, Wi-Fi status
      shownMinute = m;
      lastRefresh = millis();
      wheel.setItems(items());
      remember();
    }
  }
};

}  // namespace

Panel *makeSettingsPanel() { return new SettingsPanel(); }
Screen *makeBrightnessScreen() { return new BrightnessMode(); }
