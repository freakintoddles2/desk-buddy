// Knob speed. Each detent of the knob calls rotate(); how close together they come says
// whether the knob is being turned slowly (fine adjustment) or spun (cover ground).
#pragma once

#include <Arduino.h>

struct KnobSpeed {
  uint32_t lastAt = 0;
  int lastDir = 0;
  // Milliseconds since the previous detent in the same direction (a large number otherwise).
  uint32_t gap(int dir) {
    uint32_t now = millis();
    uint32_t g = dir == lastDir && lastAt ? now - lastAt : 100000;
    lastAt = now;
    lastDir = dir;
    return g;
  }
  bool fast(int dir) { return gap(dir) < 90; }
};

// Next multiple of `step` from v in direction dir (23 -> 25 up, 23 -> 20 down; 25 -> 30 up).
inline int nextMultiple(int v, int dir, int step) {
  if (dir > 0) return (v / step + 1) * step;
  return ((v + step - 1) / step - 1) * step;
}

// Minutes on a dial: 1 per slow click, jumps to the next multiple of 5 when turned fast.
struct MinuteKnob {
  KnobSpeed speed;
  int step(int m, int dir, int lo, int hi) {
    m = speed.fast(dir) ? nextMultiple(m, dir, 5) : m + dir;
    return constrain(m, lo, hi);
  }
};
