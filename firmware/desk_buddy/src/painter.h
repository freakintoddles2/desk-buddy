// Full-screen backgrounds painted pixel by pixel (Home's sun ring, Weather's glow) take from a
// few hundred ms to about 2 s. Painting them on the UI thread froze swipes and the knob, so:
//   - each BgCanvas keeps two 480x480 RGB565 buffers in PSRAM and shows the last finished one
//     right away when its panel is rebuilt (a swipe back to Home costs nothing);
//   - new backgrounds are painted on a background task on core 0 into the other buffer, then
//     swapped in. Only the very first paint (nothing to show yet) runs on the UI thread.
#pragma once

#include <functional>
#include <lvgl.h>
#include <stdint.h>

class BgCanvas {
 public:
  using PaintFn = std::function<void(uint16_t *buf)>;

  // Create the canvas inside root, showing the latest finished background.
  lv_obj_t *build(lv_obj_t *root);
  // Ask for the background identified by key. Nothing happens if it's already shown or being
  // painted; a newer request replaces one that hasn't started.
  void request(int64_t key, PaintFn paint);
  // Call every tick: starts queued paints and swaps finished ones in.
  void tick();
  bool ready() const { return front >= 0; }
  // Paint the very first background on the UI thread (Home at boot, before the backlight is on).
  // false: show nothing until the background task has painted it.
  bool syncFirst = true;

 private:
  uint16_t *buf[2] = {nullptr, nullptr};
  int front = -1;
  int64_t frontKey = INT64_MIN, runKey = INT64_MIN, wantKey = INT64_MIN;
  PaintFn want;
  bool running = false;
  bool noBuffers = false;  // PSRAM ran out: no background, the plain page shows
  volatile bool done = false;
  lv_obj_t *canvas = nullptr;

  void ensure();  // buffers and the painter task (first build or request)
  void show();
  void collect();  // swap in a finished paint
  void start();
  static void task(void *);
};
