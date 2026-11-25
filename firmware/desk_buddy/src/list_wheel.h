// Touch and knob (2026-11-10): a tap acts on the tapped row at once and nothing scrolls;
// the outline only shows while the knob is in use (app::knobActive), and the first turn after
// touching only brings it back.
// The scrolling list from display.html (".wheel-root"/".lrow", originally desk_display's wheel):
// rows ride up and down through a highlighted pill in the middle of the circle. The centered
// row grows, shows its subtitle, and its width follows the circle's chord. Rotate moves the
// selection; the list glides there. Touch: drag to scroll, tap a row to select it, tap the
// selected row to open it.
#pragma once

#include <Arduino.h>
#include <functional>
#include <lvgl.h>
#include <vector>

struct ListItem {
  enum Tone { Normal, Ok, Run, Add, Past };
  String id, title, badge, sub;
  Tone tone = Normal;
};

class ListWheel {
 public:
  // narrow: fraction of the circle's chord the rows use (World uses 0.8 for its ring).
  void build(lv_obj_t *root, float narrow = 0.93f);
  // Replace the items, keeping the selection on the same id when it still exists.
  void setItems(const std::vector<ListItem> &items);
  void rotate(int dir);
  void tick();  // animation; call every loop
  int selected() const { return sel; }
  const ListItem *selectedItem() const { return sel < (int)items.size() ? &items[sel] : nullptr; }
  void select(int i);
  void jump(int i);  // select without the glide (when a screen is rebuilt)
  std::function<void(int)> onPick;    // tap on the selected row
  std::function<void()> onChange;     // selection moved

  // Flat mode (Settings > Panels, 2026-11-10): every row the same size, font and
  // brightness (5 rows showing, no distance fading, no subtitle or badge), a fixed width, and
  // the wheel's centre a little lower so the top row clears the screen title. Set before build().
  bool flat = false;
  int leftInset = 0;  // flat: room before the title (a position number drawn by decorate)
  // Draws a row's own widgets (switch, arrows) into `area`, which covers the row. Called when a
  // row starts showing an item or the item's fields change. Clickable children get
  // LV_OBJ_FLAG_EVENT_BUBBLE so a drag that starts on them still scrolls the list; their click
  // handlers should ignore the click when touchMoved().
  std::function<void(lv_obj_t *area, int item, const ListItem &it)> decorate;
  std::function<void()> onTouch;  // a finger went down anywhere on the list (also on a row's button)
  bool touchMoved() const { return noTap; }
  void setLifted(bool on);   // the highlight shows a picked-up row (Reorder)
  void setHighlight(bool on);  // hide the highlight while the knob is on something else (a chip)
  bool knobShown() const { return outlineKnob; }
  // More-rows cue (Picked C in more-mockups.html, 2026-11-10): the list is cut off at the
  // bottom mid-row under a short fade, and a thin arc on the right edge shows the visible share
  // and the position, like a browser scroll bar (not touchable). World turns the arc off (its
  // 24-hour ring is on the edge). Set before build().
  bool showBar = true;

 private:
  static constexpr int POOL = 7;
  struct Row {
    lv_obj_t *box, *mk, *t, *b, *s, *x;  // x: decorate's area
    int item = -1;
    String sig;
    bool big = false;
  };
  lv_obj_t *root = nullptr, *frame = nullptr;
  Row rows[POOL];
  std::vector<ListItem> items;
  int sel = 0;
  float pos = 0, target = 0, k = 0.93f;
  float pitch = 54, extra = 30, cy = 240;
  bool outlineWanted = true, outlineKnob = false, outlineLifted = false;
  void showFrame();
  lv_obj_t *clip = nullptr, *barTrack = nullptr, *barThumb = nullptr;
  int barA = -999, barB = -999;
  void updateBar();  // row pitch, extra height of the centred row, wheel centre
  uint32_t last = 0;
  float ease = 55;  // ms time constant of the glide to `target` (longer after a flick)

  // Touch: drag follows the finger; a flick keeps going and slows down like a phone list.
  bool dragging = false;
  bool noTap = false;  // this touch moved, or stopped a moving list: its release isn't a tap
  int32_t dragStartY = 0;
  float dragStartPos = 0;
  static constexpr int SAMPLES = 6;
  struct Sample { uint32_t t; int32_t y; };
  Sample samples[SAMPLES];
  int nSamples = 0;
  void addSample(int32_t y);
  float flickVelocity();  // rows per ms, from the last ~100 ms of the drag
  void release(int32_t y);

  void layout();
  void fill(Row &r, int i, bool big);
  static void touchEvent(lv_event_t *e);
};
