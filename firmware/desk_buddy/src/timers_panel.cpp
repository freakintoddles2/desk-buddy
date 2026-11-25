// Timers panel (LISTS.timers and "timers: sub-screens and flows" in display.html): the
// stopwatch, running timers soonest first, and "+ New timer".
#include <algorithm>

#include "icons.h"
#include "list_wheel.h"
#include "theme.h"
#include "timer_face.h"
#include "timers.h"

LV_FONT_DECLARE(font_clock_80);

namespace {

// ---------- new timer: minutes on the dial ----------
struct MinutesSetup : Screen {
  bool active() const override { return true; }
  String title;
  int minutes;
  TimerFace face;
  MinuteKnob knob;
  MinutesSetup(String title, int minutes) : title(title), minutes(minutes) {}

  void build(lv_obj_t *root) override {
    face.onRing = [this](float f) { minutes = constrain((int)lroundf(f * 60), 1, 60); update(); };
    face.build(root, title.c_str(), {
      {"Cancel", ui::Tone::Normal, []() { app::pop(); }},
      {"Start", ui::Tone::Primary, [this]() { start(); }},
    }, 1);
    face.dial.setSmooth(true);
    update();
  }
  void update() {
    face.setTime(String(minutes), "min");
    face.setSub("Rotate to set");
    face.dial.set(minutes / 60.0f, false);
  }
  void rotate(int dir) override {
    minutes = knob.step(minutes, dir, 1, 60);
    update();
  }
  void press() override { start(); }
  void tick() override { face.dial.tick(); }
  bool ownsTouch() const override { return face.ringDown; }
  void start() {
    timers::add(title, minutes);
    ui::toast((title + " \xE2\x80\xA2 " + minutes + " min").c_str(), 1);
    app::drop(this);
  }
};

const struct { const char *label; int min; } PRESETS[] = {
  {"Custom...", 10}, {"Tea", 4}, {"Break", 5}, {"Pizza", 12}, {"Meeting", 25}, {"Laundry", 45},
};
constexpr int TIMER_CUSTOM = 0;

void newTimerFlow() {
  std::vector<String> opts;
  for (auto &p : PRESETS) opts.push_back(p.label);
  app::push(new PickerMode("New timer", "What for?", opts, -1, [](int i) {
    if (i == TIMER_CUSTOM) {
      app::push(new TextMode("Timer name", "", [](const String &name) { app::push(new MinutesSetup(name, 10)); }));
    } else {
      app::push(new MinutesSetup(PRESETS[i].label, PRESETS[i].min));
    }
  }));
}

// ---------- one timer ----------
struct TimerDetail : Screen {
  bool active() const override { return true; }
  uint32_t id;
  TimerFace face;
  bool builtPaused = false;
  explicit TimerDetail(uint32_t id) : id(id) {}

  void build(lv_obj_t *root) override {
    timers::Timer *t = timers::find(id);
    if (!t) { app::drop(this); return; }
    builtPaused = t->paused >= 0;
    uint32_t tid = id;
    face.build(root, t->label.c_str(), {
      {builtPaused ? "Resume" : "Pause", ui::Tone::Primary, [tid]() {
        if (auto *x = timers::find(tid)) timers::togglePause(*x);
        app::refresh();
      }},
      {"+1 min", ui::Tone::Normal, [tid]() { if (auto *x = timers::find(tid)) timers::addMinute(*x); }},
      {"Cancel timer", ui::Tone::Stop, [this, tid]() {
        timers::remove(tid);
        ui::toast("Timer cancelled");
        app::drop(this);
      }},
    }, face.sel);
    update(true);
  }
  void update(bool force = false) {
    timers::Timer *t = timers::find(id);
    if (!t) { app::drop(this); return; }  // finished (its alert is up) or cancelled
    uint32_t left = timers::left(*t);
    face.setTime(timers::clockText(left));
    face.setSub(t->paused >= 0 ? String("Paused") : String(t->ms / 60000) + " min timer");
    if (force) face.dial.set((float)left / t->ms, true);
    else face.dialEvery((float)left / t->ms, true);
  }
  void rotate(int dir) override { face.rotate(dir); }
  void press() override { face.press(); }
  void tick() override { update(); }
};

// ---------- stopwatch ----------
struct StopwatchMode : Screen {
  bool active() const override { return timers::swRunning(); }
  lv_obj_t *time = nullptr, *laps = nullptr;
  std::vector<lv_obj_t *> chips;
  int sel = 0;
  String shown;

  void build(lv_obj_t *root) override {
    ui::top(root, "Stopwatch");
    lv_obj_t *col = ui::column(root, 4);
    time = ui::label(col, "", &font_clock_80, theme::ink());
    laps = ui::label(col, "", &lv_font_montserrat_20, theme::inkDim());
    lv_obj_set_style_text_line_space(laps, 4, 0);
    lv_obj_t *r = ui::row(col, 10);
    bool run = timers::swRunning();
    const char *labels[] = {run ? "Stop" : "Start", "Lap", "Reset"};
    ui::Tone tones[] = {run ? ui::Tone::Stop : ui::Tone::Primary, ui::Tone::Normal, ui::Tone::Normal};
    chips.clear();
    for (int i = 0; i < 3; i++) {
      lv_obj_t *c = ui::chip(r, labels[i], tones[i], [](lv_event_t *e) {
        auto *self = (StopwatchMode *)lv_event_get_user_data(e);
        lv_obj_t *t = (lv_obj_t *)lv_event_get_current_target(e);
        for (int k = 0; k < 3; k++)
          if (self->chips[k] == t) { self->sel = k; self->press(); }
      }, this);
      ui::select(c, i == sel);
      chips.push_back(c);
    }
    shown = "";
    fillLaps();
    tick();
  }
  void fillLaps() {
    const auto &l = timers::swLaps();
    String s;
    int n = l.size();
    for (int i = n - 1; i >= 0 && i >= n - 3; i--) {
      if (s.length()) s += "\n";
      s += String("Lap ") + (i + 1) + "   " + timers::swText(l[i], true);
    }
    lv_label_set_text(laps, s.c_str());
  }
  void rotate(int dir) override {
    int n = constrain(sel + dir, 0, 2);
    if (n == sel) return;
    ui::select(chips[sel], false);
    sel = n;
    ui::select(chips[sel], true);
  }
  void press() override {
    if (sel == 0) { timers::swToggle(); app::refresh(); }
    else if (sel == 1) { if (timers::swLap()) fillLaps(); else ui::toast("Start it first"); }
    else { timers::swReset(); app::refresh(); }
  }
  void tick() override {
    String t = timers::swText(timers::swElapsed(), true);
    if (t != shown) { shown = t; lv_label_set_text(time, t.c_str()); }
  }
};

// ---------- the Timers list ----------
struct TimersPanel : Panel {
  bool active() const override { return !timers::all().empty() || timers::swRunning(); }
  ListWheel wheel;
  lv_obj_t *topLbl = nullptr, *counter = nullptr;
  String selId = "sw";
  uint32_t lastRefresh = 0;

  TimersPanel() : Panel("timers", "Timers", &icon_timers) {}

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    const auto &laps = timers::swLaps();
    bool run = timers::swRunning();
    v.push_back({"sw", "Stopwatch", timers::swText(timers::swElapsed(), false),
                 laps.size() ? String(laps.size()) + " laps" : String(run ? "Running" : "Stopped"),
                 run ? ListItem::Run : ListItem::Normal});
    std::vector<timers::Timer> sorted = timers::all();
    std::sort(sorted.begin(), sorted.end(), [](const timers::Timer &a, const timers::Timer &b) {
      return timers::left(a) < timers::left(b);
    });
    for (auto &t : sorted) {
      bool p = t.paused >= 0;
      v.push_back({String("t") + t.id, t.label, timers::clockText(timers::left(t)),
                   String(t.ms / 60000) + " min timer" + (p ? " \xE2\x80\xA2 paused" : ""),
                   p ? ListItem::Normal : ListItem::Run});
    }
    v.push_back({"add", "+ New timer", "", "", ListItem::Add});
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
    topLbl = ui::top(root, "");
    counter = ui::bottom(root, "");
    lv_obj_add_flag(counter, LV_OBJ_FLAG_HIDDEN);  // the scroll arc and the cut-off row replace "N / M"
    remember();
  }

  void remember() {
    if (const ListItem *it = wheel.selectedItem()) selId = it->id;
    std::vector<ListItem> its = items();
    lv_label_set_text_fmt(counter, "%d / %d", wheel.selected() + 1, (int)its.size());
    int n = timers::all().size() + (timers::swRunning() ? 1 : 0);
    lv_label_set_text_fmt(topLbl, "%d RUNNING", n);
  }

  void pick() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "sw") app::push(new StopwatchMode());
    else if (it->id == "add") newTimerFlow();
    else app::push(new TimerDetail(it->id.substring(1).toInt()));
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { pick(); }
  void tick() override {
    wheel.tick();
    if (millis() - lastRefresh >= 250) {  // live badges
      lastRefresh = millis();
      wheel.setItems(items());
      remember();
    }
  }
};

}  // namespace

Panel *makeTimersPanel() { return new TimersPanel(); }
