// The edge dial from display.html (buildDial/setDial, originally desk_display): a faint track
// around the edge, 60 ticks inside it (every 5th longer), a glowing progress arc from the top
// clockwise, and a white head dot at its end. Ticks light up as the arc passes them.
#pragma once

#include <lvgl.h>

class Dial {
 public:
  void build(lv_obj_t *root);
  // frac 0..1. amber: timer/alert color; ending: the last minute (also amber).
  void set(float frac, bool amber = false);
  // Glide toward the last set() value instead of jumping (call tick() every loop).
  void setSmooth(bool on) { smooth = on; }
  void tick();
  // Frees the shared dial image (450 KB of PSRAM) while no dial is on screen; the next dial
  // shown redraws it. Home's customize sheet needs the room for its previews.
  static void releaseCache();

 private:
  lv_obj_t *obj = nullptr;
  float shown = 0, target = 0;
  bool amber = false, smooth = false, fresh = true;
  uint32_t last = 0;
  void render();
  static void paint(lv_layer_t *layer, float f, bool amberColor);
};
