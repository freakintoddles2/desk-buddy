#include "list_wheel.h"

#include <math.h>

#include "app.h"
#include "theme.h"

namespace {

// display.html geometry (P = row pitch .096, E = extra height of the centered row .05),
// scaled up ~15% for the real screen like the rest of the UI.
constexpr float R = 240;
constexpr float P = 54;
constexpr float E = 30;

float chord(float y, float k) { return 2 * sqrtf(fmaxf(0, R * R - y * y)) * k; }

lv_color_t markColor(ListItem::Tone t) {
  switch (t) {
    case ListItem::Ok: return theme::ok();
    case ListItem::Run: return theme::today();
    default: return theme::inkFaint();
  }
}

}  // namespace

constexpr int FLAT_W = 340;

void ListWheel::build(lv_obj_t *parent, float narrow) {
  k = narrow;
  if (flat) { pitch = 50; extra = 0; cy = 252; }
  root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);  // long press still reaches the screen (back)
  lv_obj_add_event_cb(root, touchEvent, LV_EVENT_ALL, this);

  // ".sel-frame": the highlighted pill the centered row sits in.
  frame = lv_obj_create(root);
  lv_obj_remove_style_all(frame);
  lv_obj_set_size(frame, flat ? FLAT_W : (int32_t)chord(0, k), (int32_t)(pitch + extra));
  lv_obj_align(frame, LV_ALIGN_CENTER, 0, (int32_t)(cy - 240));
  lv_obj_set_style_radius(frame, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(frame, 2, 0);
  lv_obj_set_style_border_color(frame, theme::accent(), 0);
  lv_obj_set_style_bg_color(frame, lv_color_hex(0x0D111A), 0);
  lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_color(frame, theme::accent(), 0);
  lv_obj_set_style_shadow_opa(frame, LV_OPA_30, 0);
  lv_obj_set_style_shadow_width(frame, 22, 0);
  lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);
  outlineKnob = app::knobActive();  // opened with the knob: show it; with a tap: not yet
  showFrame();

  // Rows live in a window cut off mid-row at the bottom; a short fade sits over the cut.
  const int cut = flat ? 404 : 430;
  clip = lv_obj_create(root);
  lv_obj_remove_style_all(clip);
  lv_obj_set_size(clip, 480, cut);
  lv_obj_set_pos(clip, 0, 0);
  lv_obj_remove_flag(clip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *fade = lv_obj_create(root);
  lv_obj_remove_style_all(fade);
  lv_obj_set_size(fade, 480, 30);
  lv_obj_set_pos(fade, 0, cut - 30);
  lv_obj_remove_flag(fade, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(fade, theme::screen(), 0);
  lv_obj_set_style_bg_grad_color(fade, theme::screen(), 0);
  lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_main_opa(fade, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_grad_opa(fade, LV_OPA_COVER, 0);

  for (Row &r : rows) {
    r.box = lv_obj_create(clip);
    lv_obj_remove_style_all(r.box);
    lv_obj_remove_flag(r.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(r.box, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_clip_corner(r.box, false, 0);
    lv_obj_set_style_border_color(r.box, lv_color_white(), 0);
    r.mk = lv_obj_create(r.box);
    lv_obj_remove_style_all(r.mk);
    lv_obj_set_size(r.mk, 9, 9);
    lv_obj_set_style_radius(r.mk, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r.mk, LV_OPA_COVER, 0);
    r.t = lv_label_create(r.box);
    r.b = lv_label_create(r.box);
    r.s = lv_label_create(r.box);
    lv_label_set_long_mode(r.t, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(r.s, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(r.s, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(r.s, theme::inkDim(), 0);
    r.x = lv_obj_create(r.box);
    lv_obj_remove_style_all(r.x);
    lv_obj_remove_flag(r.x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r.x, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
    r.item = -1;
  }
  // The scroll bar: a track and a thumb on the right edge, 80 degrees around 3 o'clock.
  if (showBar) {
    for (lv_obj_t **a : {&barTrack, &barThumb}) {
      *a = lv_arc_create(root);
      lv_obj_remove_style_all(*a);
      lv_obj_set_size(*a, 464, 464);
      lv_obj_center(*a);
      lv_obj_remove_flag(*a, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_arc_width(*a, 4, 0);
      lv_obj_set_style_arc_rounded(*a, true, 0);
      lv_obj_set_style_arc_color(*a, lv_color_white(), 0);
      lv_obj_set_style_arc_opa(*a, *a == barTrack ? 26 : 170, 0);
      lv_arc_set_bg_angles(*a, 320, 40);
      lv_obj_add_flag(*a, LV_OBJ_FLAG_HIDDEN);
    }
  }
  last = millis();
}

void ListWheel::updateBar() {
  if (!barTrack) return;
  const float visible = flat ? 5.0f : 4.6f;  // rows on screen at once
  int n = items.size();
  bool show = n > visible;
  lv_obj_set_flag(barTrack, LV_OBJ_FLAG_HIDDEN, !show);
  lv_obj_set_flag(barThumb, LV_OBJ_FLAG_HIDDEN, !show);
  if (!show) return;
  // Thumb length is the share of the list that's visible; its place is how far down you are.
  const float span = 80, len = span * visible / n;
  float frac = fminf(1, fmaxf(0, pos / (n - 1)));
  int a = (int)lroundf(-span / 2 + (span - len) * frac), b = (int)lroundf(-span / 2 + (span - len) * frac + len);
  if (a == barA && b == barB) return;
  barA = a;
  barB = b;
  lv_arc_set_bg_angles(barThumb, (a + 360) % 360, (b + 360) % 360);
}

void ListWheel::showFrame() {
  if (frame) lv_obj_set_flag(frame, LV_OBJ_FLAG_HIDDEN, !(outlineWanted && (outlineKnob || outlineLifted)));
}

void ListWheel::setHighlight(bool on) {
  outlineWanted = on;
  showFrame();
}

void ListWheel::setLifted(bool on) {
  if (!frame) return;
  outlineLifted = on;
  showFrame();
  lv_obj_set_style_bg_color(frame, on ? theme::accent() : lv_color_hex(0x0D111A), 0);
  lv_obj_set_style_bg_opa(frame, on ? LV_OPA_30 : LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_opa(frame, on ? LV_OPA_60 : LV_OPA_30, 0);
}

void ListWheel::setItems(const std::vector<ListItem> &next) {
  // Screens refresh their items every second (clock, Wi-Fi status). If nothing changed, don't
  // relayout: layout() restyles every row, which makes LVGL redraw most of the screen.
  if (next.size() == items.size()) {
    bool same = true;
    for (size_t i = 0; same && i < next.size(); i++) {
      const ListItem &a = next[i], &b = items[i];
      same = a.id == b.id && a.title == b.title && a.badge == b.badge && a.sub == b.sub && a.tone == b.tone;
    }
    if (same) return;
  }
  String keep = sel < (int)items.size() ? items[sel].id : String();
  bool first = items.empty();
  items = next;
  int found = -1;
  for (int i = 0; i < (int)items.size(); i++)
    if (items[i].id == keep) found = i;
  sel = found >= 0 ? found : constrain(sel, 0, (int)items.size() - 1);
  if (first) pos = sel;
  target = sel;
  for (Row &r : rows) r.sig = "";  // texts may have changed
  layout();
}

void ListWheel::select(int i) {
  i = constrain(i, 0, (int)items.size() - 1);
  if (i == sel) return;
  sel = i;
  target = sel;
  if (onChange) onChange();
}

void ListWheel::jump(int i) {
  sel = constrain(i, 0, (int)items.size() - 1);
  pos = target = sel;
  layout();
}

void ListWheel::rotate(int dir) {
  if (!outlineKnob) {  // the first turn after touching only shows where the knob is
    outlineKnob = true;
    showFrame();
    return;
  }
  ease = 55;
  select(sel + dir);
}

void ListWheel::tick() {
  uint32_t now = millis();
  float dt = fminf(64, (float)(now - last));
  last = now;
  bool k = app::knobActive();
  if (k != outlineKnob && (!k || !outlineKnob)) {
    // A touch hides the outline; a knob press (not a turn) shows it again.
    outlineKnob = k;
    showFrame();
  }
  if (dragging) return;
  float diff = target - pos;
  if (fabsf(diff) < 0.003f) {
    if (pos != target) { pos = target; layout(); }
    ease = 55;
    return;
  }
  pos += diff * (1 - expf(-dt / ease));
  layout();
}

void ListWheel::fill(Row &r, int i, bool big) {
  const ListItem &it = items[i];
  String sig = it.title + "|" + it.badge + "|" + it.sub + "|" + (int)it.tone + (big ? "B" : "s");
  if (r.item == i && r.sig == sig) return;
  r.item = i;
  r.sig = sig;
  r.big = big;
  lv_label_set_text(r.t, it.title.c_str());
  lv_label_set_text(r.b, it.badge.c_str());
  lv_label_set_text(r.s, it.sub.c_str());
  // Single-line labels: a fixed one-line height makes long text end in "..." instead of wrapping.
  const lv_font_t *tf = flat ? &lv_font_montserrat_24 : big ? &lv_font_montserrat_26 : &lv_font_montserrat_22;
  lv_obj_set_style_text_font(r.t, tf, 0);
  lv_obj_set_height(r.t, lv_font_get_line_height(tf));
  lv_obj_set_height(r.s, lv_font_get_line_height(&lv_font_montserrat_18));
  lv_obj_set_style_text_font(r.b, big ? &lv_font_montserrat_20 : &lv_font_montserrat_18, 0);
  lv_color_t tc = it.tone == ListItem::Add ? theme::accent() : it.tone == ListItem::Past ? theme::inkDim()
                  : big || flat ? theme::ink() : lv_color_hex(0xD5D8DE);
  lv_obj_set_style_text_color(r.t, tc, 0);
  lv_obj_set_style_text_color(r.b, big ? theme::ink() : theme::inkDim(), 0);
  lv_obj_set_style_bg_color(r.mk, markColor(it.tone), 0);
  lv_obj_set_style_bg_opa(r.mk, it.tone == ListItem::Add ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
  if (decorate) {
    lv_obj_clean(r.x);
    decorate(r.x, i, it);
  }
}

void ListWheel::layout() {
  if (!root) return;
  int lo = max(0, (int)floorf(pos) - 3);
  int hi = min((int)items.size() - 1, (int)ceilf(pos) + 3);
  int used = 0;
  for (int i = lo; i <= hi && used < POOL; i++, used++) {
    Row &r = rows[used];
    float d = i - pos;
    float ad = fabsf(d);
    float a = fmaxf(0, 1 - ad);
    float sgn = d < 0 ? -1 : 1;
    float y = d * pitch + sgn * fminf(ad, 1) * extra / 2;
    float h = pitch + a * extra;
    float w = flat ? FLAT_W : chord(fabsf(y) + (ad < 0.5f ? 0 : h / 2 * 0.6f), k);
    fill(r, i, a > 0.5f);

    lv_obj_remove_flag(r.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(r.box, (int32_t)w, (int32_t)h);
    lv_obj_set_pos(r.box, (int32_t)(240 - w / 2), (int32_t)(cy + y - h / 2));
    lv_obj_set_size(r.x, (int32_t)w, (int32_t)h);
    if (flat) {
      // Five rows at full strength; only rows sliding in past them fade (as the faux display's
      // clipped window). Text, decorations: one opacity for the whole row's parts.
      float fade = d > 0 ? 1.0f : fminf(1, fmaxf(0, 2.9f - ad));  // below: the window cuts them off
      lv_opa_t fo = (lv_opa_t)(fade * 255);
      lv_obj_set_flag(r.box, LV_OBJ_FLAG_HIDDEN, fade <= 0);
      lv_obj_set_style_border_width(r.box, 0, 0);
      lv_obj_set_style_text_opa(r.t, fo, 0);
      lv_obj_add_flag(r.mk, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(r.s, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_flag(r.b, LV_OBJ_FLAG_HIDDEN, (bool)decorate);
      if (fade < 1) lv_obj_set_style_opa(r.x, fo, 0);
      else lv_obj_set_style_opa(r.x, LV_OPA_COVER, 0);
      int32_t th = lv_font_get_line_height(&lv_font_montserrat_24);  // the label may not be laid out yet
      int32_t tx = 18 + leftInset;
      lv_obj_set_width(r.t, (int32_t)(w - tx - 120));
      lv_obj_set_pos(r.t, tx, (int32_t)((h - th) / 2));
      continue;
    }
    // Rows above the highlight fade sooner than in display.html so they clear the top caption.
    // The fade is applied to each part's own color opacity: a whole-object `opa` would make
    // LVGL draw the row into an off-screen layer and blend it back, every frame, per row.
    float fade = d > 0 ? 1.0f : fminf(1, fmaxf(0, 2.0f - ad));  // 2 rows up: gone; below, the window cuts
    lv_opa_t fo = (lv_opa_t)(fade * 255);
    bool line = d <= -1.6f || d >= 0.6f;
    lv_obj_set_style_border_width(r.box, line ? 1 : 0, 0);
    lv_obj_set_style_border_opa(r.box, (lv_opa_t)(20 * fade), 0);
    lv_obj_set_style_text_opa(r.t, fo, 0);
    lv_obj_set_style_text_opa(r.b, fo, 0);

    // Manual grid: [mark] title / sub ... badge
    float pad = 480 * (0.045f + 0.012f * a);
    float mkW = 9 * (1 - a);
    float gap = 12 * (1 - a) + 4;
    // The value on the right (a city, a network name) gets at most 45% of the row; longer ends in "...".
    lv_label_set_long_mode(r.b, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(r.b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_update_layout(r.box);
    int32_t bw = lv_obj_get_width(r.b);
    if (bw > (int32_t)(w * 0.45f)) {
      bw = (int32_t)(w * 0.45f);
      lv_obj_set_size(r.b, bw, lv_font_get_line_height(lv_obj_get_style_text_font(r.b, LV_PART_MAIN)));  // one line
      lv_obj_update_layout(r.box);
    }
    int32_t tx = (int32_t)(pad + mkW + (mkW > 0.5f ? gap : 0));
    int32_t tw = max<int32_t>(20, (int32_t)(w - tx - pad - bw - 10));
    bool showSub = a > 0.5f && items[i].sub.length();
    int32_t th = lv_obj_get_height(r.t);
    int32_t sh = showSub ? lv_obj_get_height(r.s) : 0;
    int32_t top = (int32_t)((h - th - sh) / 2);

    lv_obj_set_style_bg_opa(r.mk, items[i].tone == ListItem::Add ? LV_OPA_TRANSP : (lv_opa_t)((1 - a) * fade * 255), 0);
    lv_obj_set_pos(r.mk, (int32_t)pad, (int32_t)(h / 2 - 4.5f));
    lv_obj_set_width(r.t, tw);
    lv_obj_set_pos(r.t, tx, top);
    lv_obj_set_pos(r.b, (int32_t)(w - pad - bw), (int32_t)(h / 2 - lv_obj_get_height(r.b) / 2));
    lv_obj_set_flag(r.s, LV_OBJ_FLAG_HIDDEN, !showSub);
    if (showSub) {
      lv_obj_set_width(r.s, tw);
      lv_obj_set_pos(r.s, tx, top + th);
      lv_obj_set_style_text_opa(r.s, (lv_opa_t)(fmaxf(0, (a - 0.5f) * 2) * fade * 255), 0);
    }
  }
  for (int u = used; u < POOL; u++) {
    lv_obj_add_flag(rows[u].box, LV_OBJ_FLAG_HIDDEN);
    rows[u].item = -1;
  }
  updateBar();
}

// ---------- touch ----------
// Like a phone list: the rows follow the finger; past either end they move at half speed and
// spring back; a flick throws the list to a row further on and it glides there.
// The throw is the fast travel tray's (the tray's sideways flick "feels exactly right"),
// in screen pixels so a flick moves the same distance under the finger: throw = v x 220 ms +
// v^2 x 17000 / 120 px (the tray's item pitch), nothing below 0.36 px/ms, 55 ms glide.

namespace {
constexpr int32_t SLOP = 8;          // px a touch can wander and still count as a tap
constexpr float THROW_MS = 220;      // as tray.cpp
constexpr float THROW_BOOST = 17000; // as tray.cpp (in its items; converted with TRAY_PITCH)
constexpr float TRAY_PITCH = 120;    // px between tray items
constexpr float THROW_MIN = 0.36f;   // px/ms (tray: 0.003 items/ms)
constexpr float GLIDE = 55;          // ms; as the tray
constexpr float LIST_THROW = 0.8f;   // lists throw a little less far than the tray (2026-11-09)
constexpr float OVER = 0.5f;         // how far past the ends (in rows) the list can be pulled

// Past the ends: half speed at first, never more than OVER rows.
float rubber(float x) { return OVER * x / (x + 2 * OVER); }
}  // namespace

void ListWheel::addSample(int32_t y) {
  if (nSamples == SAMPLES) {
    memmove(samples, samples + 1, sizeof(Sample) * (SAMPLES - 1));
    nSamples--;
  }
  samples[nSamples++] = {millis(), y};
}

float ListWheel::flickVelocity() {
  if (nSamples < 2) return 0;
  const Sample &b = samples[nSamples - 1];
  if (millis() - b.t > 80) return 0;  // the finger stopped before lifting
  int i = nSamples - 1;
  while (i > 0 && b.t - samples[i - 1].t <= 100) i--;
  const Sample &a = samples[i];
  if (b.t - a.t < 15) return 0;
  return -(float)(b.y - a.y) / (b.t - a.t) / pitch;  // finger up = list moves to later rows
}

void ListWheel::release(int32_t y) {
  if (!dragging && abs(y - dragStartY) > SLOP) {
    dragging = true;  // a flick too quick for any drag sample
    noTap = true;
  }
  if (!dragging) {
    if (noTap) {  // the touch only stopped a moving list: settle on the nearest row
      ease = 90;
      int i = constrain((int)lroundf(pos), 0, (int)items.size() - 1);
      sel = -1;
      select(i);
    }
    return;
  }
  dragging = false;
  // On release LVGL repeats the last point; sampling it again would add a still moment and
  // slow the flick (the tray uses only the finger's moves too).
  if (!nSamples || samples[nSamples - 1].y != y) addSample(y);
  float vpx = flickVelocity() * pitch;  // px/ms, positive = toward later rows
  if (fabsf(vpx) < THROW_MIN) vpx = 0;
  float throwPx = LIST_THROW * (vpx * THROW_MS + vpx * fabsf(vpx) * THROW_BOOST / TRAY_PITCH);
  int i = constrain((int)lroundf(pos + throwPx / pitch), 0, (int)items.size() - 1);
  ease = GLIDE;
  sel = -1;  // force onChange
  select(i);
}

void ListWheel::touchEvent(lv_event_t *e) {
  auto *self = (ListWheel *)lv_event_get_user_data(e);
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  int n = self->items.size();
  if (code == LV_EVENT_PRESSED) {
    if (self->onTouch) self->onTouch();
    // Touching a list that's still moving stops it, and that touch isn't a tap.
    self->noTap = fabsf(self->target - self->pos) > 0.05f;
    if (self->noTap) self->target = self->pos;
    self->dragging = false;
    self->dragStartY = p.y;
    self->dragStartPos = self->pos;
    self->nSamples = 0;
    self->addSample(p.y);
  } else if (code == LV_EVENT_PRESSING) {
    int32_t dy = p.y - self->dragStartY;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (!self->dragging && abs(dy) > SLOP && abs(v.y) >= abs(v.x)) {
      self->dragging = true;
      self->noTap = true;
    }
    self->addSample(p.y);
    if (self->dragging) {
      float raw = self->dragStartPos - (float)dy / self->pitch;
      if (raw < 0) raw = -rubber(-raw);
      else if (raw > n - 1) raw = n - 1 + rubber(raw - (n - 1));
      self->pos = self->target = raw;
      self->layout();
    }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    self->release(p.y);  // PRESS_LOST: something else took the touch; still settle the list
  } else if (code == LV_EVENT_SHORT_CLICKED) {
    // LVGL sends this right after RELEASED, so it must check what the touch did, not whether
    // a drag is still going.
    if (self->noTap) return;
    // A row's own button (Reorder's arrows) handles its tap; the click only bubbled up here.
    if (lv_event_get_target(e) != self->root) return;
    // Which row was tapped: invert the row placement around the center.
    float d = (p.y - self->cy) / (self->pitch + self->extra / 2);
    int i = (int)lroundf(self->pos + d);
    if (i < 0 || i >= n) return;
    // Act on the tapped row straight away and don't scroll (2026-11-10): the selection
    // points at it only while onPick runs (handlers read selectedItem()), then goes back to the
    // middle row, without onChange, so a rebuilt screen keeps its place.
    if (!self->onPick) return;
    int keep = self->sel;
    self->sel = i;
    self->onPick(i);
    if (self->sel == i) self->sel = keep;
  }
}
