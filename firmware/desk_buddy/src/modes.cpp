#include "modes.h"

#include "board.h"
#include "theme.h"
#include "sounds.h"

namespace {

// Chip taps carry (mode, index); the mode handles them like a press on that chip.
struct ChipTap {
  DetailMode *mode;
  int index;
};

}  // namespace

// ---------- DetailMode ----------

void DetailMode::build(lv_obj_t *root) {
  if (topText.length()) ui::top(root, topText.c_str());
  lv_obj_t *col = ui::column(root, gap);
  if (shiftY) lv_obj_align(col, LV_ALIGN_CENTER, 0, shiftY);
  if (body)
    for (const Line &l : body()) {
      lv_obj_t *lb = ui::label(col, l.text.c_str(), l.font, l.color);
      lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_WRAP);
      lv_obj_set_width(lb, 360);
    }
  if (extra) extra(col);
  ctls = controls ? controls() : std::vector<Control>{};
  sel = constrain(sel, 0, max(0, (int)ctls.size() - 1));
  chips.clear();
  if (!ctls.empty()) {
    lv_obj_t *r = ui::row(col);
    for (int i = 0; i < (int)ctls.size(); i++) {
      lv_obj_t *c = ui::chip(r, ctls[i].label.c_str(), ctls[i].tone, [](lv_event_t *e) {
        auto *self = (DetailMode *)lv_event_get_user_data(e);
        lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(e);
        for (int k = 0; k < (int)self->chips.size(); k++)
          if (self->chips[k] == target) { self->sel = k; self->press(); }
      }, this);
      ui::select(c, i == sel);
      chips.push_back(c);
    }
  }
}

void DetailMode::rotate(int dir) {
  if (chips.empty()) return;
  int n = constrain(sel + dir, 0, (int)chips.size() - 1);
  if (n == sel) return;
  ui::select(chips[sel], false);
  sel = n;
  ui::select(chips[sel], true);
}

void DetailMode::press() {
  if (sel < (int)ctls.size() && ctls[sel].fn) {
    auto fn = ctls[sel].fn;  // copy: fn may drop this mode
    fn();
  }
}

// ---------- PickerMode ----------

PickerMode::PickerMode(String title, String heading, std::vector<String> options, int current,
                       std::function<void(int)> onPick)
    : title(title), heading(heading), options(options), current(current),
      idx(constrain(current, 0, (int)options.size() - 1)), onPick(onPick) {}

void PickerMode::build(lv_obj_t *root) {
  wheel = ListWheel();
  wheel.build(root);
  std::vector<ListItem> items;
  for (int k = 0; k < (int)options.size(); k++) {
    ListItem it;
    it.id = String(k);
    it.title = options[k];
    if (k == current) {
      it.badge = LV_SYMBOL_OK;  // the current setting
      it.tone = ListItem::Ok;
    }
    items.push_back(it);
  }
  wheel.setItems(items);
  wheel.jump(idx);
  wheel.onPick = [this](int i) { choose(i); };
  wheel.onChange = [this]() {
    int i = wheel.selected();
    if (i != idx && onHover) onHover(i);
    idx = i;
  };
  ui::top(root, title.c_str());  // the list itself makes the heading ("Pick one") redundant
}

void PickerMode::rotate(int dir) { wheel.rotate(dir); }

void PickerMode::tick() { wheel.tick(); }

void PickerMode::press() { choose(wheel.selected()); }

void PickerMode::choose(int i) {
  auto cb = onPick;
  app::drop(this);
  cb(i);
}

// ---------- FieldsMode ----------

FieldsMode::FieldsMode(String title, std::vector<Field> fields,
                       std::function<String(const std::vector<Field> &)> preview,
                       std::function<void(const std::vector<Field> &)> onDone,
                       std::function<void(std::vector<Field> &)> fix)
    : title(title), fields(fields), preview(preview), onDone(onDone), fix(fix) {}

void FieldsMode::build(lv_obj_t *root) {
  ui::top(root, title.c_str());
  lv_obj_t *col = ui::column(root, 10);
  lv_obj_t *r = lv_obj_create(col);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(r, 10, 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
  boxes.clear();
  values.clear();
  for (int i = 0; i < (int)fields.size(); i++) {
    lv_obj_t *box = lv_obj_create(r);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(box, 74, 0);
    lv_obj_set_style_pad_hor(box, 12, 0);
    lv_obj_set_style_pad_ver(box, 8, 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(box, (void *)(intptr_t)i);
    lv_obj_add_event_cb(box, [](lv_event_t *e) {
      auto *self = (FieldsMode *)lv_event_get_user_data(e);
      self->idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
      self->update();
    }, LV_EVENT_CLICKED, this);
    values.push_back(ui::label(box, "", &lv_font_montserrat_36, theme::ink()));
    lv_obj_t *l = ui::label(box, ui::upper(fields[i].label.c_str()), &lv_font_montserrat_14, theme::inkDim());
    lv_obj_set_style_text_letter_space(l, 2, 0);
    boxes.push_back(box);
  }
  previewLabel = ui::label(col, "", &lv_font_montserrat_20, theme::inkDim());
  lv_obj_t *btns = ui::row(col);
  ui::chip(btns, "Cancel", ui::Tone::Normal, [](lv_event_t *) { app::pop(); }, nullptr);
  ui::chip(btns, "Save", ui::Tone::Primary, [](lv_event_t *e) {
    ((FieldsMode *)lv_event_get_user_data(e))->done();
  }, this);
  update();
}

void FieldsMode::update() {
  for (int i = 0; i < (int)fields.size(); i++) {
    const Field &f = fields[i];
    lv_label_set_text(values[i], f.fmt ? f.fmt(f.value).c_str() : String(f.value).c_str());
    bool on = i == idx;
    lv_obj_set_style_border_color(boxes[i], on ? theme::accent() : theme::inkFaint(), 0);
    lv_obj_set_style_shadow_color(boxes[i], theme::accent(), 0);
    lv_obj_set_style_shadow_opa(boxes[i], on ? LV_OPA_40 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(boxes[i], on ? 18 : 0, 0);
  }
  if (preview) lv_label_set_text(previewLabel, preview(fields).c_str());
}

void FieldsMode::rotate(int dir) {
  Field &f = fields[idx];
  int mx = f.maxFn ? f.maxFn(fields) : f.max;
  int span = mx - f.min + 1;
  int v = f.value + dir;
  if (speed.fast(dir) && f.fastStep) {
    v = nextMultiple(f.value, dir, f.fastStep);
    if (v > mx) v = f.min;                                // past the top: wrap to the start
    else if (v < f.min) v = f.value == f.min ? mx / f.fastStep * f.fastStep : f.min;  // stop at the start, then wrap
  }
  f.value = f.min + ((((v - f.min) % span) + span) % span);
  if (fix) fix(fields);
  update();
}

void FieldsMode::press() {
  if (idx < (int)fields.size() - 1) {
    idx++;
    update();
  } else {
    done();
  }
}

void FieldsMode::done() {
  auto cb = onDone;
  auto vals = fields;
  app::drop(this);
  cb(vals);
}

// ---------- TextMode ----------

namespace {
// Ring order from display.html: done, delete, space, A-Z, 0-9, dash, apostrophe, period ("St. John's").
const char *const KEYS[] = {
  LV_SYMBOL_OK, LV_SYMBOL_BACKSPACE, "_",
  "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
  "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "'", ".",
};
constexpr int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);
constexpr int KEY_DONE = 0, KEY_DEL = 1, KEY_SPACE = 2;
constexpr float KEY_R = 0.44f * 480;
constexpr int KEY_SIZE = 34;
constexpr int BUBBLE = 50;  // the highlight, ~1.5x a key
}  // namespace

TextMode::TextMode(String title, String value, std::function<void(const String &)> onDone, int max)
    : title(title), text(value), onDone(onDone), max(max) {}

void TextMode::build(lv_obj_t *root) {
  keys.clear();
  bubble = nullptr;
  for (int i = 0; i < NKEYS; i++) {
    float a = (float)i / NKEYS * 2 * M_PI;
    lv_obj_t *k = lv_obj_create(root);
    lv_obj_remove_style_all(k);
    lv_obj_set_size(k, KEY_SIZE, KEY_SIZE);
    lv_obj_set_pos(k, (int32_t)(240 + KEY_R * sinf(a) - KEY_SIZE / 2), (int32_t)(240 - KEY_R * cosf(a) - KEY_SIZE / 2));
    lv_obj_set_style_radius(k, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(k, theme::accent(), 0);
    lv_obj_set_style_shadow_color(k, theme::accent(), 0);
    lv_obj_set_style_transform_pivot_x(k, KEY_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(k, KEY_SIZE / 2, 0);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(k, 3);
    lv_obj_set_user_data(k, (void *)(intptr_t)i);
    lv_obj_add_event_cb(k, [](lv_event_t *e) {
      auto *self = (TextMode *)lv_event_get_user_data(e);
      int i = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
      self->styleKey(self->idx, false);
      self->idx = i;
      self->styleKey(i, true);
      self->put(i);
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *l = ui::label(k, KEYS[i], &lv_font_montserrat_20, theme::inkDim());
    lv_obj_center(l);
    keys.push_back(k);
    styleKey(i, false);
  }
  // The highlight: one bubble above the ring that moves from key to key. (Restyling the key
  // itself and raising it to the front made LVGL redraw the whole screen, ~54 ms per click.)
  bubble = lv_obj_create(root);
  lv_obj_remove_style_all(bubble);
  lv_obj_set_size(bubble, BUBBLE, BUBBLE);
  lv_obj_set_style_radius(bubble, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(bubble, theme::accent(), 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_color(bubble, theme::accent(), 0);
  lv_obj_set_style_shadow_opa(bubble, LV_OPA_70, 0);
  lv_obj_set_style_shadow_width(bubble, 14, 0);
  lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);  // taps reach the key underneath
  bubbleText = ui::label(bubble, "", &lv_font_montserrat_26, lv_color_white());
  lv_obj_center(bubbleText);
  styleKey(idx, true);

  lv_obj_t *col = ui::column(root, 6);
  lv_obj_t *t = ui::label(col, ui::upper(title.c_str()), &lv_font_montserrat_18, theme::inkDim());
  lv_obj_set_style_text_letter_space(t, 4, 0);
  // The entry line: the text, and a delete key anchored at its right end (deleting is common,
  // so it gets a big touch target here as well as its key on the ring). Holding it repeats.
  lv_obj_t *line = lv_obj_create(col);
  lv_obj_remove_style_all(line);
  lv_obj_set_size(line, 300, LV_SIZE_CONTENT);
  lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_border_side(line, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(line, 1, 0);
  lv_obj_set_style_border_color(line, theme::inkFaint(), 0);
  lv_obj_set_style_pad_bottom(line, 4, 0);
  entry = ui::label(line, "", &lv_font_montserrat_34, theme::ink());
  lv_label_set_long_mode(entry, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(entry, 236);
  lv_obj_set_style_text_align(entry, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(entry, LV_ALIGN_LEFT_MID, 8, 0);
  delKey = lv_obj_create(line);
  lv_obj_remove_style_all(delKey);
  lv_obj_set_size(delKey, 52, 40);
  lv_obj_align(delKey, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_radius(delKey, 12, 0);
  lv_obj_set_style_bg_color(delKey, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(delKey, 22, 0);
  lv_obj_set_style_bg_opa(delKey, 60, LV_STATE_PRESSED);
  lv_obj_remove_flag(delKey, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(delKey, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(delKey, 10);
  lv_obj_t *dl = ui::label(delKey, LV_SYMBOL_BACKSPACE, &lv_font_montserrat_22, theme::ink());
  lv_obj_center(dl);
  // PRESSED deletes at once; held, LONG_PRESSED_REPEAT keeps deleting (LVGL's repeat rate).
  lv_obj_add_event_cb(delKey, [](lv_event_t *e) {
    ((TextMode *)lv_event_get_user_data(e))->put(KEY_DEL);
  }, LV_EVENT_PRESSED, this);
  lv_obj_add_event_cb(delKey, [](lv_event_t *e) {
    ((TextMode *)lv_event_get_user_data(e))->put(KEY_DEL);
  }, LV_EVENT_LONG_PRESSED_REPEAT, this);
  count = ui::label(col, "", &lv_font_montserrat_18, theme::inkDim());
  lv_obj_t *btns = ui::row(col);
  ui::chip(btns, "Cancel", ui::Tone::Normal, [](lv_event_t *) { app::pop(); }, nullptr);
  ui::chip(btns, "Done", ui::Tone::Primary, [](lv_event_t *e) {
    ((TextMode *)lv_event_get_user_data(e))->finish();
  }, this);
  update();
}

void TextMode::styleKey(int i, bool on) {
  if (i < 0 || i >= (int)keys.size()) return;
  lv_obj_t *k = keys[i];
  bool fn = i <= KEY_SPACE;
  // The key's own label hides under the bubble so its edges don't peek out.
  lv_obj_set_style_text_color(lv_obj_get_child(k, 0), fn ? theme::accent() : theme::inkDim(), 0);
  lv_obj_set_style_text_opa(lv_obj_get_child(k, 0), on ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
  if (!on || !bubble) return;
  float a = (float)i / NKEYS * 2 * M_PI;  // same spot as the key (see build)
  lv_obj_set_pos(bubble, (int32_t)(240 + KEY_R * sinf(a) - BUBBLE / 2), (int32_t)(240 - KEY_R * cosf(a) - BUBBLE / 2));
  lv_label_set_text(bubbleText, KEYS[i]);
}

void TextMode::update() {
  String shown = text + "|";  // caret
  lv_label_set_text(entry, shown.c_str());
  lv_obj_set_style_text_opa(lv_obj_get_child(delKey, 0), text.length() ? LV_OPA_COVER : 70, 0);  // dim when empty
  lv_label_set_text_fmt(count, "%d / %d", (int)text.length(), max);
}

// Same feel as Ring pong's paddle: a slow click moves one key; clicks coming fast move up to
// 4 keys each, right away (a queue that played the keys out one by one lagged behind the knob).
void TextMode::rotate(int dir) {
  uint32_t gap = speed.gap(dir);
  int n = gap < 150 ? (int)lroundf(constrain(1 + (150 - (int)gap) / 35.0f, 1.0f, 4.0f)) : 1;
  styleKey(idx, false);
  idx = ((idx + dir * n) % NKEYS + NKEYS) % NKEYS;
  styleKey(idx, true);
}

void TextMode::press() { put(idx); }

void TextMode::put(int i) {
  if (i == KEY_DONE) { finish(); return; }
  if (i == KEY_DEL) {
    if (text.length()) { text.remove(text.length() - 1); sounds::play(sounds::KEY_DELETE); }
    update();
    return;
  }
  if ((int)text.length() >= max) { ui::toast("20 characters max", -1); return; }
  if (i == KEY_SPACE) {
    if (!text.length() || text.endsWith(" ")) return;
    text += ' ';
  } else {
    String c = KEYS[i];
    if (text.length()) c.toLowerCase();  // sentence case
    text += c;
  }
  sounds::play(sounds::KEY_TYPE);
  update();
}

void TextMode::finish() {
  String t = text;
  t.trim();
  if (!t.length()) { ui::toast("Type a name first", -1); return; }
  auto cb = onDone;
  app::drop(this);
  cb(t);
}

// ---------- AlertMode ----------

AlertMode::AlertMode(String top, String title, String sub, std::vector<Control> actions,
                     const lv_font_t *titleFont) {
  topText = top;
  body = [title, sub, titleFont]() {
    std::vector<Line> v{{title, titleFont, theme::ink()}};
    if (sub.length()) v.push_back({sub, &lv_font_montserrat_22, theme::inkDim()});
    return v;
  };
  // Every action also closes the alert.
  std::vector<Control> acts;
  for (auto a : actions) {
    auto fn = a.fn;
    acts.push_back({a.label, a.tone, [this, fn]() {
      app::drop(this);
      if (fn) fn();
    }});
  }
  controls = [acts]() { return acts; };
}

void AlertMode::build(lv_obj_t *root) {
  // An amber ring around the edge, like the faux display's alert dial.
  lv_obj_t *ring = lv_obj_create(root);
  lv_obj_remove_style_all(ring);
  lv_obj_set_size(ring, 464, 464);
  lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, 6, 0);
  lv_obj_set_style_border_color(ring, theme::today(), 0);
  lv_obj_set_style_shadow_color(ring, theme::today(), 0);
  lv_obj_set_style_shadow_width(ring, 24, 0);
  lv_obj_set_style_shadow_opa(ring, LV_OPA_40, 0);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  DetailMode::build(root);
  if (!started) started = millis();
}

void AlertMode::tick() {
  // LED blinks three times when the alert appears, then stays on until it's answered.
  uint32_t t = millis() - started;
  board::setLed(t > 1800 || (t / 300) % 2 == 0);
}
