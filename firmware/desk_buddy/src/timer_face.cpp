#include "timer_face.h"

#include <math.h>

#include "theme.h"

LV_FONT_DECLARE(font_clock_96);

void TimerFace::build(lv_obj_t *root, const char *labelText, std::vector<Control> controls, int selected) {
  dial = Dial();
  dial.build(root);
  lastDial = 0;
  shownTime = shownSub = "";

  // Ring touch: a full-screen catcher under the text and chips. Anything outside the middle
  // counts (the ring sits at ~225 px; the edge of a round screen is hard to hit exactly).
  ringDown = ringDrag = false;
  if (onRing) {
    lv_obj_t *catcher = lv_obj_create(root);
    lv_obj_remove_style_all(catcher);
    lv_obj_set_size(catcher, 480, 480);
    lv_obj_set_pos(catcher, 0, 0);
    lv_obj_remove_flag(catcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(catcher, ringEvent, LV_EVENT_ALL, this);
  }

  lv_obj_t *col = ui::column(root, 2);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);  // let presses beside the text reach the ring
  label = ui::label(col, ui::upper(labelText), &lv_font_montserrat_18, theme::inkDim());
  lv_obj_set_style_text_letter_space(label, 4, 0);
  ui::makeBack(label);  // Brightness, Day max, new timer: "< BRIGHTNESS" goes back

  lv_obj_t *tr = lv_obj_create(col);  // time + unit on one baseline
  lv_obj_remove_style_all(tr);
  lv_obj_set_size(tr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(tr, 8, 0);
  lv_obj_remove_flag(tr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(tr, LV_OBJ_FLAG_CLICKABLE);
  time = ui::label(tr, "", &font_clock_96, theme::ink());
  unit = ui::label(tr, "", &lv_font_montserrat_26, theme::inkDim());
  lv_obj_set_style_pad_bottom(unit, 22, 0);
  lv_obj_add_flag(unit, LV_OBJ_FLAG_HIDDEN);

  sub = ui::label(col, "", &lv_font_montserrat_20, theme::inkDim());

  ctls = controls;
  sel = constrain(selected, 0, max(0, (int)ctls.size() - 1));
  chips.clear();
  if (!ctls.empty()) {
    lv_obj_t *r = ui::row(col, 10);
    for (int i = 0; i < (int)ctls.size(); i++) {
      lv_obj_t *c = ui::chip(r, ctls[i].label.c_str(), ctls[i].tone, [](lv_event_t *e) {
        auto *self = (TimerFace *)lv_event_get_user_data(e);
        lv_obj_t *t = (lv_obj_t *)lv_event_get_current_target(e);
        for (int k = 0; k < (int)self->chips.size(); k++)
          if (self->chips[k] == t) { self->sel = k; self->press(); }
      }, this);
      ui::select(c, i == sel);
      chips.push_back(c);
    }
  }
}

// A touch that starts in the ring band is held as "pending" until it shows what it is:
// moving along the ring (tangentially) drags the value; moving across it (radially, like a
// swipe from the edge) lets the swipe through; lifting without moving is a tap that sets the
// value at that point.
void TimerFace::ringEvent(lv_event_t *e) {
  constexpr float RING_MIN = 110;  // ring at ~225 px; everything outside the text counts (150 was too tight)
  constexpr float DECIDE = 10;     // px of movement before deciding
  auto *self = (TimerFace *)lv_event_get_user_data(e);
  lv_event_code_t code = lv_event_get_code(e);
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  float dx = p.x - 240.0f, dy = p.y - 240.0f;
  auto apply = [&](bool dragging) {
    float f = atan2f(dx, -dy) / (2 * (float)M_PI);
    if (f < 0) f += 1;
    // Past the top while dragging: stay at whichever end it was near.
    if (dragging && fabsf(f - self->ringFrac) > 0.5f) f = self->ringFrac > 0.5f ? 1.0f : 0.0f;
    self->ringFrac = f;
    self->onRing(f);
  };
  switch (code) {
    case LV_EVENT_PRESSED:
      self->ringDown = hypotf(dx, dy) >= RING_MIN;
      self->ringDrag = false;
      self->downX = p.x;
      self->downY = p.y;
      break;
    case LV_EVENT_PRESSING: {
      if (!self->ringDown) break;
      if (self->ringDrag) { apply(true); break; }
      float mx = p.x - self->downX, my = p.y - self->downY;
      if (hypotf(mx, my) < DECIDE) break;
      float r = hypotf(self->downX - 240.0f, self->downY - 240.0f);
      float rx = (self->downX - 240.0f) / r, ry = (self->downY - 240.0f) / r;
      float radial = fabsf(mx * rx + my * ry), tangential = fabsf(-mx * ry + my * rx);
      if (tangential >= radial) {
        self->ringDrag = true;
        dx = self->downX - 240.0f;  // start from where the finger landed, then follow it
        dy = self->downY - 240.0f;
        apply(false);
        dx = p.x - 240.0f;
        dy = p.y - 240.0f;
        apply(true);
      } else {
        self->ringDown = false;  // a swipe: let LVGL's gesture have it
      }
      break;
    }
    case LV_EVENT_RELEASED:
      if (self->ringDown && !self->ringDrag) apply(false);  // a tap on the ring
      self->ringDown = self->ringDrag = false;
      break;
    case LV_EVENT_PRESS_LOST:
      self->ringDown = self->ringDrag = false;
      break;
    default:
      break;
  }
}

void TimerFace::setTime(const String &t, const char *unitText) {
  if (t != shownTime) {
    shownTime = t;
    lv_label_set_text(time, t.c_str());
  }
  // Only touch the unit label when it changes: setting a label's text invalidates it even when
  // the text is the same, and this runs every frame (Focus idled at 32 redraws a second).
  bool hide = !unitText;
  if (lv_obj_has_flag(unit, LV_OBJ_FLAG_HIDDEN) != hide) lv_obj_set_flag(unit, LV_OBJ_FLAG_HIDDEN, hide);
  if (unitText && strcmp(lv_label_get_text(unit), unitText) != 0) lv_label_set_text(unit, unitText);
}

void TimerFace::setSub(const String &s) {
  if (s == shownSub) return;
  shownSub = s;
  lv_label_set_text(sub, s.c_str());
}

void TimerFace::rotate(int dir) {
  if (chips.empty()) return;
  int n = constrain(sel + dir, 0, (int)chips.size() - 1);
  if (n == sel) return;
  ui::select(chips[sel], false);
  sel = n;
  ui::select(chips[sel], true);
}

void TimerFace::press() {
  if (sel < (int)ctls.size() && ctls[sel].fn) {
    auto fn = ctls[sel].fn;
    fn();
  }
}

void TimerFace::dialEvery(float frac, bool amber) {
  if (millis() - lastDial < 250 && lastDial) return;
  lastDial = millis();
  dial.set(frac, amber);
}
