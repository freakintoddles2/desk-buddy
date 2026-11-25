#include "ui.h"

#include "app.h"
#include "theme.h"

namespace ui {

namespace {
lv_obj_t *backTitle = nullptr;
}

void makeBack(lv_obj_t *l) {
  if (!app::canGoBack()) return;
  // The chevron is the label's first glyph (LV_SYMBOL_LEFT is in the built-in fonts), so the
  // pair stays centred together whatever the title's length.
  String t = String(LV_SYMBOL_LEFT " ") + lv_label_get_text(l);
  lv_label_set_text(l, t.c_str());
  lv_obj_set_style_text_color(l, lv_color_hex(0xC9CFDA), 0);
  lv_obj_set_style_pad_hor(l, 12, 0);
  lv_obj_set_style_pad_ver(l, 5, 0);
  lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(l, theme::accent(), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(l, LV_OPA_30, LV_STATE_PRESSED);
  lv_obj_set_style_text_color(l, lv_color_white(), LV_STATE_PRESSED);
  lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(l, 14);
  lv_obj_add_event_cb(l, [](lv_event_t *) { app::back(); }, LV_EVENT_CLICKED, nullptr);
  backTitle = l;
}

void clearBack() { backTitle = nullptr; }

void raiseBack() {
  if (!backTitle) return;
  // Lift the title (or the dial column holding it) above later full-screen objects such as a
  // list's touch area. Non-clickable containers still let touches through to what's below.
  lv_obj_t *o = backTitle;
  while (lv_obj_get_parent(o) && lv_obj_get_parent(lv_obj_get_parent(o)) &&
         lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(o))))
    o = lv_obj_get_parent(o);  // stop at the screen root's child (scr > root > child)
  lv_obj_move_foreground(o);
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  return l;
}

lv_obj_t *top(lv_obj_t *root, const char *text) {
  lv_obj_t *l = label(root, upper(text), &lv_font_montserrat_18, theme::inkDim());
  lv_obj_set_style_text_letter_space(l, 3, 0);
  lv_obj_update_layout(l);
  if (lv_obj_get_width(l) > 380) {  // a long place or network name: keep it inside the circle
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, 380);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  }
  makeBack(l);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, app::canGoBack() ? 87 : 92);  // the back title's padding
  return l;
}

lv_obj_t *bottom(lv_obj_t *root, const char *text) {
  lv_obj_t *l = label(root, text, &lv_font_montserrat_18, theme::inkDim());
  lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -30);
  return l;
}

// The selected chip's ring and glow draw up to ~20 px outside the chip. A row or column is
// sized to its chips, so LVGL (which draws the screen in strips and skips containers outside a
// strip) cut the ring off along the bottom. Let these containers draw past their edges.
void roomForGlow(lv_obj_t *o) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_add_event_cb(o, [](lv_event_t *e) { lv_event_set_ext_draw_size(e, 20); }, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
  lv_obj_refresh_ext_draw_size(o);
}

lv_obj_t *column(lv_obj_t *root, int gap, bool belowTop) {
  lv_obj_t *col = lv_obj_create(root);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, 400, LV_SIZE_CONTENT);
  if (belowTop) {
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 128);
  } else {
    lv_obj_center(col);
  }
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, gap, 0);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
  roomForGlow(col);
  return col;
}

lv_obj_t *row(lv_obj_t *parent, int gap) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, 340, LV_SIZE_CONTENT);  // fixed width so a third chip wraps inside the circle
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, gap, 0);
  lv_obj_set_style_pad_row(r, gap, 0);
  lv_obj_set_style_pad_top(r, 14, 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
  roomForGlow(r);
  return r;
}

lv_obj_t *chip(lv_obj_t *parent, const char *text, Tone tone, lv_event_cb_t cb, void *user) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(c, 22, 0);
  lv_obj_set_style_pad_ver(c, 9, 0);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_color_t border = theme::inkFaint(), text_c = theme::inkDim();
  if (tone == Tone::Primary) { border = theme::accent(); text_c = theme::ink(); }
  if (tone == Tone::Stop) { border = theme::overdue(); text_c = theme::overdue(); }
  if (tone == Tone::Done) { border = theme::ok(); text_c = theme::ok(); }
  lv_obj_set_style_border_color(c, border, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  if (cb) {
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(c, 8);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, user);
  } else {
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_t *l = label(c, text, &lv_font_montserrat_22, text_c);
  lv_obj_center(l);
  return c;
}

void select(lv_obj_t *c, bool on) {
  // ".ctl.sel": 2 px accent ring plus a soft accent glow.
  lv_obj_set_style_outline_width(c, on ? 2 : 0, 0);
  lv_obj_set_style_outline_color(c, theme::accent(), 0);
  lv_obj_set_style_outline_pad(c, 0, 0);
  lv_obj_set_style_shadow_color(c, theme::accent(), 0);
  lv_obj_set_style_shadow_opa(c, on ? LV_OPA_40 : LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(c, on ? 18 : 0, 0);
}

namespace {
lv_obj_t *toastObj = nullptr;
lv_timer_t *toastTimer = nullptr;
}  // namespace

void toast(const char *text, int tone) {
  if (!toastObj) {
    toastObj = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(toastObj);
    lv_obj_set_size(toastObj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(toastObj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(toastObj, 18, 0);
    lv_obj_set_style_pad_ver(toastObj, 8, 0);
    lv_obj_set_style_bg_opa(toastObj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(toastObj, LV_OBJ_FLAG_CLICKABLE);
    label(toastObj, "", &lv_font_montserrat_22, theme::ink());
    lv_obj_align(toastObj, LV_ALIGN_TOP_MID, 0, 88);
  }
  // Solid tints (rgba over the dark screen in display.html, pre-blended here).
  uint32_t bg = tone > 0 ? 0x173A2A : tone < 0 ? 0x40222A : 0x2A2D33;
  lv_obj_set_style_bg_color(toastObj, lv_color_hex(bg), 0);
  lv_label_set_text(lv_obj_get_child(toastObj, 0), text);
  lv_obj_remove_flag(toastObj, LV_OBJ_FLAG_HIDDEN);
  if (toastTimer) lv_timer_delete(toastTimer);
  toastTimer = lv_timer_create([](lv_timer_t *) {
    lv_obj_add_flag(toastObj, LV_OBJ_FLAG_HIDDEN);
    toastTimer = nullptr;
  }, 1800, nullptr);
  lv_timer_set_repeat_count(toastTimer, 1);
  Serial.printf("[toast] %s\n", text);
}

const char *upper(const char *s) {
  static char buf[64];
  size_t n = 0;
  for (; *s && n < sizeof(buf) - 1; s++) buf[n++] = toupper((unsigned char)*s);
  buf[n] = 0;
  return buf;
}

}  // namespace ui
