#include "tz.h"
#include "reminders.h"

#include <math.h>
#include <time.h>

#include "app.h"
#include "clock.h"
#include "board.h"
#include "modes.h"
#include "remind_icons.h"
#include "theme.h"
#include "ui.h"
#include "sounds.h"

namespace reminders {
namespace {

const Info INFO[store::REMINDER_COUNT] = {
  {"Stand-up reminder", "Stand up", "Stretch your legs for a minute", "stand up"},
  {"Water reminder", "Drink some water", "A few sips counts", "drink some water"},
  {"Eye break", "Eye break", "Look 20 feet away for 20 seconds", "rest your eyes"},
};
// Each reminder's color and icon (reminder-mockups.html option B).
const uint32_t COLOR[store::REMINDER_COUNT] = {0x3FCF7F, 0x4F8CFF, 0xA66BFF};
const lv_image_dsc_t *const ICON[store::REMINDER_COUNT] = {&remind_stand, &remind_water, &remind_eyes};

constexpr uint32_t TOGETHER_MS = 2 * 60000UL;  // due this close together: shown together
constexpr uint32_t COUNTDOWN_MS = 30000;       // the card leaves by itself after this
constexpr uint32_t INTRO_MS = 380;             // the ring fills in as the card appears

uint32_t due[store::REMINDER_COUNT] = {0, 0, 0};  // millis(); 0 = off
uint32_t lastCheck = 0;
bool wasActive = false;

bool isDue(int i, uint32_t now, uint32_t ahead = 0) { return due[i] && (int32_t)(now + ahead - due[i]) >= 0; }

void resetAll() {
  for (int i = 0; i < store::REMINDER_COUNT; i++) reset((store::Reminder)i);
}

// When the card goes: anything that came due while it was up is covered by it.
void resetOverdue() {
  uint32_t now = millis();
  for (int i = 0; i < store::REMINDER_COUNT; i++)
    if (isDue(i, now)) reset((store::Reminder)i);
}

// The reminder card: big icon and words in the reminder's color, one action (dismiss: swipe
// either way, tap, or press), and a ring at the edge that fills in, then drains over 30 s;
// when it's empty the card goes away by itself. Several reminders due together share the
// card (dots; turn the knob to step through them) and one countdown.
// The ring is an lv_arc: changing its angle only redraws the thin slice that moved, so the
// countdown costs a few hundred pixels a frame, not a full-screen redraw.
class ReminderCard : public Screen {
 public:
  explicit ReminderCard(std::vector<int> w) : which(std::move(w)) {}
  bool isAlert() const override { return true; }

  void build(lv_obj_t *root) override {
    int r = which[idx];
    lv_color_t c = lv_color_hex(COLOR[r]);
    arc = lv_arc_create(root);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, 468, 468);
    lv_obj_center(arc);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(arc, 270);  // 0 degrees at the top
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, c, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    shownEnd = ringEnd();
    lv_arc_set_angles(arc, 0, shownEnd);

    lv_obj_t *col = ui::column(root, 10);
    lv_obj_t *disc = lv_obj_create(col);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, 116, 116);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, c, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_20, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *img = lv_image_create(disc);
    lv_image_set_src(img, ICON[r]);
    lv_obj_set_style_image_recolor(img, c, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);

    ui::label(col, INFO[r].title, &lv_font_montserrat_40, theme::ink());
    lv_obj_t *sub = ui::label(col, INFO[r].sub, &lv_font_montserrat_22, theme::inkDim());
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(sub, 330);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    if (which.size() > 1) {
      lv_obj_t *dots = ui::row(col, 8);
      for (size_t k = 0; k < which.size(); k++) {
        lv_obj_t *d = lv_obj_create(dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 9, 9);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, (int)k == idx ? theme::ink() : theme::inkFaint(), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
      }
    }
    lv_obj_t *r2 = ui::row(col);
    lv_obj_t *chip = ui::chip(r2, "Dismiss", ui::Tone::Primary, [](lv_event_t *e) {
      ((ReminderCard *)lv_event_get_user_data(e))->dismiss();
    }, this);
    ui::select(chip, true);
  }

  void tick() override {
    uint32_t t = elapsed();
    board::setLed(t > 1800 || (t / 300) % 2 == 0);  // three blinks, then on until it goes
    if (t >= INTRO_MS + COUNTDOWN_MS) {
      Serial.println("[reminder] counted down; gone");
      finish();
      return;
    }
    float e = ringEnd();
    if (arc && fabsf(e - shownEnd) >= 0.05f) {  // ~0.2 px at the rim
      shownEnd = e;
      lv_arc_set_angles(arc, 0, e);
    }
  }

  void rotate(int dir) override {
    if (which.size() < 2) return;
    int n = constrain(idx + dir, 0, (int)which.size() - 1);
    if (n != idx) { idx = n; app::refresh(); }
  }
  void press() override { dismiss(); }
  bool swipe(int) override { dismiss(); return true; }

  void dismiss() {
    which.erase(which.begin() + idx);
    if (which.empty()) { finish(); return; }
    idx = min(idx, (int)which.size() - 1);
    app::refresh();
  }

 private:
  std::vector<int> which;
  int idx = 0;
  uint32_t started = 0;
  lv_obj_t *arc = nullptr;
  float shownEnd = 0;
  bool done = false;

  uint32_t elapsed() {
    if (!started) started = millis();
    return millis() - started;
  }
  // Ring end angle: fills 0 -> 360 (ease-out) as the card appears, then drains to 0.
  float ringEnd() {
    uint32_t t = elapsed();
    if (t < INTRO_MS) {
      float p = 1.0f - (float)t / INTRO_MS;
      return fmaxf(0.1f, 360.0f * (1.0f - p * p * p));
    }
    float left = 1.0f - (float)(t - INTRO_MS) / COUNTDOWN_MS;
    return fmaxf(0.1f, 360.0f * left);
  }
  void finish() {
    if (done) return;
    done = true;
    resetOverdue();
    app::drop(this);
  }
};

void fire(const std::vector<int> &which) {
  for (int r : which) reset((store::Reminder)r);
  for (int r : which) Serial.printf("[reminder] %s\n", INFO[r].title);
  if (!which.empty()) {
    static const sounds::Event EV[] = {sounds::REMIND_STAND, sounds::REMIND_WATER, sounds::REMIND_EYES};
    sounds::play(EV[constrain(which[0], 0, 2)]);
  }
  app::pushAlert(new ReminderCard(which));
}

}  // namespace

const Info &info(store::Reminder r) { return INFO[r]; }

void fireNow(store::Reminder r) { fire({(int)r}); }

void allDueNow() {
  for (int i = 0; i < store::REMINDER_COUNT; i++)
    if (due[i]) due[i] = millis();
  Serial.printf("[reminder] all due now; hours %s, %s\n", hoursText(hours()).c_str(), activeNow() ? "active" : "not active");
}

void begin() {
  resetAll();
  wasActive = activeNow();
}

void reset(store::Reminder r) {
  uint8_t m = store::reminderMinutes(r);
  due[r] = m ? millis() + m * 60000UL : 0;
  if (due[r] == 0 && m) due[r] = 1;
}

Hours hours() {
  return {store::getInt("remall", 0) != 0, constrain(store::getInt("remfrom", 8), 0, 23),
          constrain(store::getInt("remto", 18), 0, 23), store::getInt("remdays", 1) != 0};
}

void setHours(const Hours &h) {
  store::setInt("remall", h.allDay);
  store::setInt("remfrom", h.from);
  store::setInt("remto", h.to);
  store::setInt("remdays", h.weekdays);
  wasActive = false;  // re-evaluated on the next tick; entering the hours restarts the countdowns
}

static String hourText(int h) { return tz::hourText(h); }

String hoursText(const Hours &h) { return h.allDay ? String("Any time") : hourText(h.from) + "-" + hourText(h.to); }
String daysText(const Hours &h) { return h.allDay ? String("Day and night") : h.weekdays ? String("Weekdays") : String("Every day"); }

bool activeNow() {
  Hours h = hours();
  if (h.allDay) return true;
  time_t t = time(nullptr);
  if (!clockx::trusted()) return true;  // clock not set yet: its hour means nothing, don't silence reminders
  struct tm l;
  localtime_r(&t, &l);
  if (h.weekdays && (l.tm_wday == 0 || l.tm_wday == 6)) return false;
  int m = l.tm_hour * 60 + l.tm_min;
  int a = h.from * 60, b = h.to * 60;
  if (a == b) return true;
  return a < b ? (m >= a && m < b) : (m >= a || m < b);
}

void tick() {
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();
  bool act = activeNow();
  if (!act) {
    wasActive = false;
    return;
  }
  if (!wasActive) {
    // The reminder hours just started (or changed): first reminders one interval from now.
    wasActive = true;
    resetAll();
    return;
  }
  if (app::busy()) return;
  uint32_t now = millis();
  bool any = false;
  for (int i = 0; i < store::REMINDER_COUNT; i++) any |= isDue(i, now);
  if (!any) return;
  std::vector<int> which;
  for (int i = 0; i < store::REMINDER_COUNT; i++)
    if (isDue(i, now, TOGETHER_MS)) which.push_back(i);
  fire(which);
}

}  // namespace reminders
