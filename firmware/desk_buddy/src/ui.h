// Shared building blocks, styled after display.html (sizes scaled up for the real screen).
#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace ui {

enum class Tone { Normal, Primary, Stop, Done };

// Centered label.
lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

// Caption across the top of a screen (".top": 19.5% down, small caps, dim).
lv_obj_t *top(lv_obj_t *root, const char *text);
// Back chevron (Picked A in back-mockups.html, 2026-11-10): on a sub-screen, top() and the
// dial screens' caps label read "< TITLE", a bit brighter, and tapping them goes back.
void makeBack(lv_obj_t *titleLabel);
void clearBack();  // app: before building a screen
void raiseBack();  // app: after building it
// Small dim line near the bottom (".bottom").
lv_obj_t *bottom(lv_obj_t *root, const char *text);

// A centered flex column; belowTop pushes it down to clear the top caption.
lv_obj_t *column(lv_obj_t *root, int gap = 10, bool belowTop = false);
lv_obj_t *row(lv_obj_t *parent, int gap = 12);
// Let a container's children (chip rings and glows) draw up to 20 px past its edges.
void roomForGlow(lv_obj_t *o);

// A pill-shaped control. Taps call cb(user). select() shows the knob highlight.
lv_obj_t *chip(lv_obj_t *parent, const char *text, Tone tone, lv_event_cb_t cb, void *user);
void select(lv_obj_t *chip, bool on);

// Brief message near the top of the screen. good/bad tint it green/red.
void toast(const char *text, int tone = 0);  // 0 neutral, 1 good, -1 bad

// "Hello" -> "HELLO" into a static buffer (labels copy their text).
const char *upper(const char *s);

}  // namespace ui
