// Reusable sub-screens from display.html: detailMode, pickerMode, fieldsMode, textMode, alerts.
#pragma once

#include "knob.h"
#include "list_wheel.h"

#include <Arduino.h>
#include <functional>
#include <vector>

#include "app.h"

// A chip control on a sub-screen.
struct Control {
  String label;
  ui::Tone tone = ui::Tone::Normal;
  std::function<void()> fn;
};

// A line of text in a detail screen's body.
struct Line {
  String text;
  const lv_font_t *font;
  lv_color_t color;
};

// Body text plus a row of chips. Rotate moves between chips; press or tap activates.
class DetailMode : public Screen {
 public:
  String topText;
  std::function<std::vector<Line>()> body;
  // Custom widgets between the body text and the chips (a habit's week, ...).
  std::function<void(lv_obj_t *col)> extra;
  int gap = 8;     // space between body items
  int shiftY = 0;  // move the content down (positive) from the screen's center
  std::function<std::vector<Control>()> controls;
  int sel = 0;
  void build(lv_obj_t *root) override;
  void rotate(int dir) override;
  void press() override;

 protected:
  std::vector<lv_obj_t *> chips;
  std::vector<Control> ctls;
};

// Pick one of a list, in the same scrolling list as the panels (rotate or drag/flick to move,
// press or tap the highlighted row to choose). The current setting carries a check.
class PickerMode : public Screen {
 public:
  PickerMode(String title, String heading, std::vector<String> options, int current,
             std::function<void(int)> onPick);
  void build(lv_obj_t *root) override;
  void rotate(int dir) override;
  void press() override;
  void tick() override;
  // Called when the highlight moves to another option (a sound picker plays it).
  std::function<void(int)> onHover;

 private:
  String title, heading;
  std::vector<String> options;
  int current, idx;
  std::function<void(int)> onPick;
  ListWheel wheel;
  void choose(int i);
};

// Several numbers set one after another (date, time): rotate changes the highlighted field,
// press moves to the next one, and the last press saves.
struct Field {
  String label;
  int value, min, max;
  std::function<int(const std::vector<Field> &)> maxFn;  // e.g. days in the month
  std::function<String(int)> fmt;
  int fastStep = 0;  // turned fast, jump to the next multiple of this (0: always 1 at a time)
};

class FieldsMode : public Screen {
 public:
  FieldsMode(String title, std::vector<Field> fields,
             std::function<String(const std::vector<Field> &)> preview,
             std::function<void(const std::vector<Field> &)> onDone,
             std::function<void(std::vector<Field> &)> fix = nullptr);
  void build(lv_obj_t *root) override;
  void rotate(int dir) override;
  void press() override;
  bool isEntry() const override { return true; }

 private:
  String title;
  std::vector<Field> fields;
  std::function<String(const std::vector<Field> &)> preview;
  std::function<void(const std::vector<Field> &)> onDone;
  std::function<void(std::vector<Field> &)> fix;
  int idx = 0;
  KnobSpeed speed;
  std::vector<lv_obj_t *> boxes, values;
  lv_obj_t *previewLabel = nullptr;
  void update();
  void done();
};

// Text entry without a keyboard: the characters sit around the round edge. Rotate moves the
// highlight around the ring, press types it. Done and delete are on the ring too, so the knob
// alone can do everything; touch can tap any character directly.
class TextMode : public Screen {
 public:
  TextMode(String title, String value, std::function<void(const String &)> onDone, int max = 20);
  void build(lv_obj_t *root) override;
  void rotate(int dir) override;
  void press() override;
  bool isEntry() const override { return true; }

 private:
  String title, text;
  KnobSpeed speed;  // spun fast, the highlight moves several keys per click
  std::function<void(const String &)> onDone;
  int max, idx = 3;
  std::vector<lv_obj_t *> keys;
  lv_obj_t *entry = nullptr, *count = nullptr, *delKey = nullptr;
  lv_obj_t *bubble = nullptr, *bubbleText = nullptr;
  void put(int i);
  void finish();
  void update();
  void styleKey(int i, bool on);
};

// An alert (reminder, timer done): blinks the LED and waits for an answer. Hold doesn't
// dismiss it; one of its chips does.
class AlertMode : public DetailMode {
 public:
  AlertMode(String top, String title, String sub, std::vector<Control> actions,
            const lv_font_t *titleFont = &lv_font_montserrat_40);
  void build(lv_obj_t *root) override;
  void tick() override;
  bool isAlert() const override { return true; }

 private:
  uint32_t started = 0;
};
