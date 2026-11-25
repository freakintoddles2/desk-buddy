// Focus panel (renderFocus/focusRotate in display.html). Idle: rotate sets the length (1-60 min:
// 1 per slow click, 5s when turned fast; the dial shows it out of an hour), press starts.
// Running: the dial counts down (amber in the last minute); rotate picks Pause/Resume or Stop,
// press does it.
#include "icons.h"
#include "timer_face.h"
#include "timers.h"

namespace {

struct FocusPanel : Panel {
  bool active() const override { return timers::focusActive(); }  // idle return waits
  TimerFace face;
  MinuteKnob knob;
  bool builtActive = false, builtPaused = false;

  FocusPanel() : Panel("focus", "Focus", &icon_focus) {}

  void build(lv_obj_t *root) override {
    builtActive = timers::focusActive();
    builtPaused = timers::focusPaused();
    std::vector<Control> c;
    if (builtActive) {
      c.push_back({builtPaused ? "Resume" : "Pause", ui::Tone::Primary, []() { timers::toggleFocusPause(); app::refresh(); }});
      c.push_back({"Stop", ui::Tone::Stop, []() { timers::stopFocus(); ui::toast("Focus stopped"); app::refresh(); }});
    } else {
      c.push_back({"Start", ui::Tone::Primary, []() { timers::startFocus(); app::refresh(); }});
    }
    face.onRing = nullptr;
    if (!builtActive)
      face.onRing = [this](float f) { timers::setFocusMinutes(constrain((int)lroundf(f * 60), 1, 60)); update(); };
    face.build(root, builtActive ? "Focus" : "Focus timer", c, 0);
    face.dial.setSmooth(!builtActive);
    update(true);
  }

  void update(bool force = false) {
    if (timers::focusActive() != builtActive || timers::focusPaused() != builtPaused) {
      app::refresh();  // finished or changed elsewhere
      return;
    }
    if (!builtActive) {
      int m = timers::focusMinutes();
      face.setTime(String(m), "min");
      int n = timers::sessionsToday();
      face.setSub(n ? String(n) + (n == 1 ? " session today" : " sessions today") : String("No sessions yet today"));
      face.dial.set(m / 60.0f, false);
    } else {
      uint32_t left = timers::focusLeft();
      face.setTime(timers::clockText(left));
      face.setSub(builtPaused ? String("Paused") : String(timers::focusLength() / 60000) + " min session");
      float frac = (float)left / timers::focusLength();
      if (force) face.dial.set(frac, left < 60000);
      else face.dialEvery(frac, left < 60000);
    }
  }

  void rotate(int dir) override {
    if (builtActive) { face.rotate(dir); return; }
    timers::setFocusMinutes(knob.step(timers::focusMinutes(), dir, 1, 60));
    update();
  }
  void press() override { face.press(); }
  bool ownsTouch() const override { return face.ringDown; }
  void tick() override {
    face.dial.tick();
    update();
  }
};

}  // namespace

Panel *makeFocusPanel() { return new FocusPanel(); }
