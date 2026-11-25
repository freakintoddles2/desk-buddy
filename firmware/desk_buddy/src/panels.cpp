// Checkpoint 1 panels: placeholders that exercise the framework.
// Each real panel replaces its placeholder in a later checkpoint (see BUILD_PLAN.md).
#include "panels.h"

#include "board.h"
#include "icons.h"
#include "store.h"
#include "modes.h"
#include "net.h"
#include "theme.h"

namespace {

// Column of content centered on the round screen.
lv_obj_t *column(lv_obj_t *root, int gap = 10) {
  lv_obj_t *col = lv_obj_create(root);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, 380, LV_SIZE_CONTENT);
  lv_obj_center(col);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, gap, 0);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
  ui::roomForGlow(col);
  return col;
}

// ---------- a sub-screen, to test the mode stack ----------
struct DemoDetail : Screen {
  const char *panelName;
  int value = 0;
  int sel = 0;  // 0 = Back chip (only control)
  lv_obj_t *valueLabel = nullptr;
  explicit DemoDetail(const char *name) : panelName(name) {}

  void build(lv_obj_t *root) override {
    lv_obj_t *col = column(root, 12);
    lv_obj_t *t = ui::label(col, panelName, &lv_font_montserrat_20, theme::inkDim());
    lv_obj_set_style_text_letter_space(t, 2, 0);
    ui::label(col, "Sub-screen", &lv_font_montserrat_34, theme::ink());
    valueLabel = ui::label(col, "", &lv_font_montserrat_48, theme::ink());
    ui::label(col, "Rotate to change\nHold or swipe to go back", &lv_font_montserrat_20, theme::inkDim());
    ui::select(ui::chip(col, "Back", ui::Tone::Primary, [](lv_event_t *) { app::pop(); }, nullptr), true);
    update();
  }
  void update() { lv_label_set_text_fmt(valueLabel, "%d", value); }
  void rotate(int dir) override { value += dir; update(); }
  void press() override { app::pop(); }  // the only control is Back
};

// ---------- generic placeholder ----------
struct Placeholder : Panel {
  int checkpoint;
  int knob = 0;
  lv_obj_t *knobLabel = nullptr;
  Placeholder(const char *id, const char *name, const lv_image_dsc_t *icon, int checkpoint)
      : Panel(id, name, icon), checkpoint(checkpoint) {}

  void build(lv_obj_t *root) override {
    lv_obj_t *col = column(root);
    ui::label(col, name, &lv_font_montserrat_48, theme::ink());
    char note[48];
    snprintf(note, sizeof(note), "Coming in checkpoint %d", checkpoint);
    ui::label(col, note, &lv_font_montserrat_20, theme::inkDim());
    knobLabel = ui::label(col, "", &lv_font_montserrat_30, theme::accent());
    ui::label(col, "Press for a sub-screen", &lv_font_montserrat_20, theme::inkFaint());
    update();
  }
  void update() { lv_label_set_text_fmt(knobLabel, "Knob %d", knob); }
  void rotate(int dir) override { knob += dir; update(); }
  void press() override { app::push(new DemoDetail(name)); }
};

}  // namespace

std::vector<Panel *> makePanels() {
  // Default swipe order from display.html.
  return {
    makeHomePanel(),
    makeWeatherPanel(),  // before World (2026-11-10)
    makeWorldPanel(),
    makeFocusPanel(),
    makeTimersPanel(),
    makeBreathePanel(),
    makeHabitsPanel(),
    makeDaysPanel(),
    makePlayPanel(),
    makeSettingsPanel(),
  };
}
