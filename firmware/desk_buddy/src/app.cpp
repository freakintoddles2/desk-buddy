#include "autodim.h"
#include "app.h"

#include <algorithm>

#include "board.h"
#include "store.h"
#include "theme.h"
#include "timers.h"
#include "tray.h"
#include "sounds.h"

namespace app {
namespace {

std::vector<Panel *> allPanels;  // default order
std::vector<Screen *> modes;
Panel *current = nullptr;

lv_obj_t *scr = nullptr;
lv_obj_t *root = nullptr;      // current panel or sub-screen widgets
lv_obj_t *nav = nullptr;       // panel nav strip window
lv_obj_t *navTrack = nullptr;  // row of icons that slides inside the window
uint32_t lastGestureAt = 0;
uint32_t lastInputAt = 0;  // knob, button or touch
bool knobUsed = false;      // knob or button more recent than touch
int idleMin = 2;
uint32_t touchDownAt = 0;
lv_point_t touchDownPt;
bool touchHomeFired = false;

// Screen changes are applied from loop(), never inside an LVGL event callback: the callback's
// own object (a chip, a nav pill) would otherwise be deleted while LVGL is still using it.
bool renderPending = false;
int pendingSlide = 0;
std::vector<Screen *> graveyard;  // removed sub-screens, deleted after the current event

Screen *top() { return modes.empty() ? (Screen *)current : modes.back(); }

void noScroll(lv_obj_t *o) {
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

// ---------- nav strip ----------

void navClicked(lv_event_t *e);

void buildNav() {
  if (!nav) {
    nav = lv_obj_create(scr);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, theme::NAV_W, theme::NAV_H);
    lv_obj_align(nav, LV_ALIGN_TOP_MID, 0, theme::NAV_Y);
    noScroll(nav);
  }
  lv_obj_clean(nav);
  lv_obj_set_flag(nav, LV_OBJ_FLAG_HIDDEN, !modes.empty());
  if (!modes.empty()) return;

  navTrack = lv_obj_create(nav);
  lv_obj_remove_style_all(navTrack);
  lv_obj_set_size(navTrack, LV_SIZE_CONTENT, theme::NAV_H);
  lv_obj_set_flex_flow(navTrack, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(navTrack, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(navTrack, theme::NAV_GAP, 0);
  noScroll(navTrack);

  lv_obj_t *onItem = nullptr;
  for (Panel *p : shownPanels()) {
    bool on = p == current;
    // Icons only (Picked B in nav-mockups.html, 2026-11-10): the current panel's icon is
    // larger and white with a soft blue glow; no pill, no name.
    lv_obj_t *pill = lv_obj_create(navTrack);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, on ? theme::NAV_PILL_H : LV_SIZE_CONTENT, theme::NAV_PILL_H);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 6, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(pill, on ? 0 : 5, 0);
    lv_obj_set_style_pad_right(pill, on ? 0 : 5, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(pill, 6);
    lv_obj_add_event_cb(pill, navClicked, LV_EVENT_CLICKED, p);
    if (on) {
      lv_obj_set_style_shadow_color(pill, theme::accent(), 0);
      // LVGL skips the shadow of a fully transparent object, so the disc is barely there (3%).
      lv_obj_set_style_bg_color(pill, theme::accent(), 0);
      lv_obj_set_style_bg_opa(pill, 8, 0);
      lv_obj_set_style_shadow_opa(pill, LV_OPA_50, 0);
      lv_obj_set_style_shadow_width(pill, 24, 0);
      lv_obj_set_style_shadow_spread(pill, -4, 0);
      onItem = pill;
    }

    lv_obj_t *img = lv_image_create(pill);
    lv_image_set_src(img, p->icon);
    lv_obj_set_style_image_recolor(img, on ? lv_color_white() : theme::ink(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(img, on ? LV_OPA_COVER : LV_OPA_30, 0);  // other panels fainter
    if (on) lv_image_set_scale(img, 320);  // 24 -> 30 px
  }

  // Slide the row so the current pill sits in the middle; fade icons toward the window edges
  // (the faux display masks 0-22% and 78-100% of the window with a gradient).
  lv_obj_update_layout(navTrack);
  if (onItem) {
    int32_t x = theme::NAV_W / 2 - (lv_obj_get_x(onItem) + lv_obj_get_width(onItem) / 2);
    lv_obj_set_x(navTrack, x);
    uint32_t cnt = lv_obj_get_child_count(navTrack);
    for (uint32_t i = 0; i < cnt; i++) {
      lv_obj_t *c = lv_obj_get_child(navTrack, i);
      int32_t center = x + lv_obj_get_x(c) + lv_obj_get_width(c) / 2;
      float t = (float)center / theme::NAV_W;
      float edge = t < 0.5f ? t : 1.0f - t;
      float a = edge >= 0.22f ? 1.0f : (edge <= 0 ? 0.0f : edge / 0.22f);
      lv_obj_set_style_opa(c, (lv_opa_t)(a * 255), 0);
    }
  }
}

// ---------- rendering ----------

void slideIn(lv_obj_t *obj, int dir) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_values(&a, dir > 0 ? -14 : 14, 0);
  lv_anim_set_duration(&a, 140);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t *)o, v, 0); });
  lv_anim_start(&a);
}

void doRender(int slideDir) {
  std::vector<Panel *> shown = shownPanels();
  if (std::find(shown.begin(), shown.end(), current) == shown.end()) current = shown.front();
  if (root) lv_obj_delete(root);
  root = lv_obj_create(scr);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, board::SCREEN_W, board::SCREEN_H);
  noScroll(root);
  lv_obj_move_background(root);  // keep the nav strip on top
  ui::clearBack();
  top()->build(root);
  ui::raiseBack();  // the back title above anything built after it (full-screen lists)
  buildNav();
  if (slideDir) slideIn(root, slideDir);
}

void render(int slideDir = 0) {
  renderPending = true;
  if (slideDir) pendingSlide = slideDir;
}

void clearModes() {
  while (!modes.empty()) {
    graveyard.push_back(modes.back());
    modes.pop_back();
  }
}

void goPanel(Panel *p) {
  std::vector<Panel *> shown = shownPanels();
  int from = std::find(shown.begin(), shown.end(), current) - shown.begin();
  int to = std::find(shown.begin(), shown.end(), p) - shown.begin();
  clearModes();
  current = p;
  Serial.printf("[app] panel %s\n", p->id);
  render(to > from ? 1 : -1);
}

void navClicked(lv_event_t *e) {
  if (millis() - lastGestureAt < 300) return;  // the end of a swipe isn't a tap
  Panel *p = (Panel *)lv_event_get_user_data(e);
  if (p != current || !modes.empty()) goPanel(p);
}

// ---------- input ----------

void swipe(int dir) {
  Serial.printf("[input] swipe %s\n", dir > 0 ? "next" : "prev");
  if (!modes.empty()) {
    Screen *m = modes.back();
    if (m->noSwipe()) return;
    if (!m->swipe(dir)) pop();
    return;
  }
  std::vector<Panel *> shown = shownPanels();
  int pos = std::find(shown.begin(), shown.end(), current) - shown.begin();
  int next = constrain(pos + dir, 0, (int)shown.size() - 1);
  if (shown[next] == current) return;
  current = shown[next];
  Serial.printf("[app] panel %s\n", current->id);
  render(dir);
}

void longPress() {
  Serial.println("[input] hold");
  if (!modes.empty() && !modes.back()->isAlert()) {
    sounds::play(sounds::BACK);
    pop();
  }
}

void screenEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (top()->ownsTouch()) return;  // dragging a dial ring isn't a swipe or a hold
  if (code == LV_EVENT_GESTURE) {
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_LEFT || d == LV_DIR_RIGHT) {
      lastGestureAt = millis();
      lv_indev_wait_release(lv_indev_active());
      swipe(d == LV_DIR_LEFT ? 1 : -1);
    }    else if (d == LV_DIR_TOP || d == LV_DIR_BOTTOM) {
      // Only a screen that uses the swipe ends the touch. Otherwise a list's flick would lose
      // its release (LVGL sends PRESS_LOST instead) and stop dead where the finger left.
      int16_t sx, sy;
      board::touchStart(sx, sy);
      lv_indev_t *indev = lv_indev_active();
      if (top()->swipeVertical(d == LV_DIR_TOP ? -1 : 1, sy)) {
        lastGestureAt = millis();
        lv_indev_wait_release(indev);
      }
    }
  } else if (code == LV_EVENT_LONG_PRESSED) {
    longPress();
  }
}

// Apply a pending rebuild. Called before any screen method runs, so a screen never touches
// widgets from a build that has already been deleted.
void flush() {
  if (renderPending) {
    renderPending = false;
    int dir = pendingSlide;
    pendingSlide = 0;
    doRender(dir);
  }
  for (Screen *s : graveyard) delete s;
  graveyard.clear();
}

}  // namespace

namespace {

// A finger held still for HOME_HOLD_MS goes Home, like holding the knob. (Holding 0.7 s
// already backs out of a sub-screen; this is the longer hold.) Moving cancels it, so a slow
// list drag never counts.
void watchTouch(uint32_t now) {
  lv_indev_t *t = board::touchIndev();
  if (!t) return;
  if (lv_indev_get_state(t) != LV_INDEV_STATE_PRESSED) {
    touchDownAt = 0;
    touchHomeFired = false;
    return;
  }
  lastInputAt = now;
  autodim::input();
  knobUsed = false;
  lv_point_t p;
  lv_indev_get_point(t, &p);
  if (!touchDownAt) {
    touchDownAt = now;
    touchDownPt = p;
    return;
  }
  if (abs(p.x - touchDownPt.x) > 12 || abs(p.y - touchDownPt.y) > 12) {
    touchDownAt = now;  // moved: start over from here
    touchDownPt = p;
    return;
  }
  // Not on text/number entry: a finger held on a key (delete repeats) mustn't throw the entry
  // away. The knob's hold, Cancel and a swipe still leave.
  if (top()->isEntry()) return;
  if (!touchHomeFired && now - touchDownAt >= board::HOME_HOLD_MS) {
    touchHomeFired = true;
    lv_indev_wait_release(t);  // the release isn't a tap
    goHome();
  }
}

// After idleMin minutes with no input, slide back to Home, unless something is going on:
// an alert, a game, text or number entry, or a screen that's running something.
void idleCheck(uint32_t now) {
  if (!idleMin || now - lastInputAt < (uint32_t)idleMin * 60000) return;
  lastInputAt = now;  // check again after another full idle period
  for (Screen *m : modes)
    if (m->isAlert() || m->isGame() || m->isEntry() || m->active()) return;
  if (current->active()) return;
  Serial.println("[app] idle");
  goHome();
}

}  // namespace

void begin(const std::vector<Panel *> &panels) {
  allPanels = panels;
  current = allPanels.front();
  scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, theme::screen(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, screenEvent, LV_EVENT_GESTURE, nullptr);
  lv_obj_add_event_cb(scr, screenEvent, LV_EVENT_LONG_PRESSED, nullptr);
  idleMin = store::getInt("idlehome", 2);
  lastInputAt = millis();
  doRender(0);
}

void push(Screen *mode) {
  modes.push_back(mode);
  render();
}

void pop() {
  if (modes.empty()) return;
  graveyard.push_back(modes.back());
  modes.pop_back();
  render();
}

void drop(Screen *mode) {
  auto it = std::find(modes.begin(), modes.end(), mode);
  if (it == modes.end()) return;
  modes.erase(it);
  graveyard.push_back(mode);
  render();
}

void pushAlert(Screen *alert) {
  auto entry = std::find_if(modes.begin(), modes.end(), [](Screen *s) { return s->isEntry(); });
  modes.insert(entry, alert);
  board::setLed(true);
  render();
}

void refresh() { render(); }

void goTo(const char *id) {
  for (Panel *p : shownPanels())
    if (!strcmp(p->id, id)) { goPanel(p); return; }
}

void injectSwipe(int dir) {
  if (tray::live()) { tray::swipe(dir); return; }
  swipe(dir);
}

void injectSwipeVertical(int dir, int startY) { top()->swipeVertical(dir, startY); }

Panel *currentPanel() { return current; }

void jump(Panel *p) {
  clearModes();
  current = p;
  Serial.printf("[app] panel %s\n", p->id);
  renderPending = false;
  pendingSlide = 0;
  doRender(0);
  for (Screen *s : graveyard) delete s;
  graveyard.clear();
}

void pushNow(Screen *mode) {
  modes.push_back(mode);
  Serial.println("[app] push now");
  renderPending = false;
  pendingSlide = 0;
  doRender(0);
}

Screen *topScreen() { return top(); }

// Only from a top-level panel: in any sub-screen (Brightness, a picker, a detail, a game, an
// alert) the pull got in the way (2026-11-10).
bool fastTravelAllowed() { return modes.empty(); }

bool alertWaiting() {
  for (Screen *m : modes)
    if (m->isAlert()) return true;
  return false;
}

std::vector<Panel *> orderedPanels() {
  std::vector<Panel *> out;
  for (const String &id : store::panelOrder())
    for (Panel *p : allPanels)
      if (id == p->id && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  for (Panel *p : allPanels)
    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  return out;
}

std::vector<Panel *> shownPanels() {
  std::vector<Panel *> out;
  for (Panel *p : orderedPanels())
    if ((locked(p) || !store::hidden(p->id)) && p->visible()) out.push_back(p);
  return out;
}

bool knobActive() { return knobUsed; }

bool canGoBack() { return !modes.empty() && !modes.back()->isAlert(); }

void back() {
  if (!canGoBack()) return;
  Serial.println("[input] back (title)");
  pop();
}

void goHome() {
  for (Screen *m : modes)
    if (m->isAlert()) return;  // an alert must be answered first
  for (Panel *p : allPanels)
    if (!strcmp(p->id, "home")) {
      if (p == current && modes.empty()) return;
      Serial.println("[app] home");
      goPanel(p);
      return;
    }
}

int idleMinutes() { return idleMin; }
void setIdleMinutes(int m) {
  idleMin = m;
  store::setInt("idlehome", m);
}

bool locked(const Panel *p) { return !strcmp(p->id, "home") || !strcmp(p->id, "settings"); }

bool busy() {
  for (Screen *m : modes)
    if (m->isAlert() || m->isGame()) return true;
  return timers::focusRunning();
}

void loop() {
  flush();  // changes requested by LVGL events (taps, swipes) during the last lv_timer_handler()
  tray::loop();
  if (tray::live()) {
    // The tray has the screen, the touch, the knob and the button until it slides away.
    uint32_t t = millis();
    lastInputAt = t;
    autodim::input();
    int32_t st = board::takeKnobSteps();
    if (st) sounds::play(sounds::KNOB_TICK);
    while (st != 0) {
      int dir = st > 0 ? 1 : -1;
      tray::rotate(dir);
      st -= dir;
    }
    switch (board::pollButton()) {
      case board::Button::Press: sounds::play(sounds::KNOB_PRESS); tray::press(); break;
      case board::Button::LongPress: tray::hold(); break;
      case board::Button::HoldHome: tray::home(); break;
      default: break;
    }
    top()->tick();
    flush();
    for (Panel *p : allPanels)
      if (p != top()) p->background();
    return;
  }
  uint32_t now = millis();
  int32_t steps = board::takeKnobSteps();
  if (steps) {
    lastInputAt = now; autodim::input(); knobUsed = true;
    if (!top()->isGame()) sounds::play(sounds::KNOB_TICK);  // games make their own sounds
  }
  while (steps != 0) {
    int dir = steps > 0 ? 1 : -1;
    top()->rotate(dir);
    steps -= dir;
    flush();
  }
  board::Button btn = board::pollButton();
  if (btn != board::Button::None) { lastInputAt = now; autodim::input(); knobUsed = true; }
  switch (btn) {
    case board::Button::Down:
      top()->pressDown(board::downAt());
      break;
    case board::Button::HoldHome:
      if (!(current && !strcmp(current->id, "home") && modes.empty())) sounds::play(sounds::HOME);
      goHome();
      break;
    case board::Button::Press:
      Serial.println("[input] press");
      if (!top()->isGame()) sounds::play(sounds::KNOB_PRESS);  // a screen's own sound (dice, a letter) replaces it
      top()->press();
      break;
    case board::Button::LongPress:
      longPress();
      break;
    default:
      break;
  }
  flush();
  watchTouch(now);
  idleCheck(now);
  flush();
  top()->tick();
  flush();
  for (Panel *p : allPanels)
    if (p != top()) p->background();
  bool alertUp = false;
  for (Screen *m : modes) alertUp |= m->isAlert();
  if (!alertUp) board::setLed(false);
}

}  // namespace app
