// The layout shared by Focus, a timer's detail screen and the new-timer setup: the edge dial,
// then a small caps label, a big time, a dim line and a row of chips in the middle.
#pragma once

#include <functional>
#include <vector>

#include "dial.h"
#include "knob.h"
#include "modes.h"

class TimerFace {
 public:
  Dial dial;
  lv_obj_t *label = nullptr, *time = nullptr, *unit = nullptr, *sub = nullptr;
  std::vector<lv_obj_t *> chips;
  std::vector<Control> ctls;
  int sel = 0;

  // Set before build() to let a finger set the value on the ring: tap a point or drag along it.
  // Gets 0..1 clockwise from the top; dragging past the top stops at the end (no wrap).
  std::function<void(float)> onRing;
  bool ringDown = false;  // a finger is on the ring (the screen's ownsTouch())
  bool ringDrag = false;
  int downX = 0, downY = 0;

  void build(lv_obj_t *root, const char *labelText, std::vector<Control> controls, int selected);
  void setTime(const String &t, const char *unitText = nullptr);
  void setSub(const String &s);
  void rotate(int dir);
  void press();
  // Update the dial at most every 250 ms (each update redraws the whole ring).
  void dialEvery(float frac, bool amber);

 private:
  String shownTime, shownSub;
  float ringFrac = 0;
  static void ringEvent(lv_event_t *e);
  uint32_t lastDial = 0;
};
