// Break reminders (stand, water, eyes). Each fires every N minutes (Settings), but only
// within the reminder hours (Settings > Reminder hours, default weekdays 8 AM to 6 PM), and
// waits while something shouldn't be interrupted (focus session, game, another alert).
// A reminder shows as a full-screen card (big icon and words in its own color) with one
// action, dismiss; a ring at the edge counts down 30 s and then it goes away by itself. No
// snooze and no counting: another one is always coming. Reminders due within 2 minutes of
// each other share the card.
#pragma once

#include <Arduino.h>

#include "store.h"

namespace reminders {

struct Info {
  const char *name, *title, *sub, *phrase;  // phrase: for combined alerts ("drink some water")
};
const Info &info(store::Reminder r);

void begin();
void reset(store::Reminder r);  // restart the countdown (after a change or an answer)
void tick();                    // call from loop()
void fireNow(store::Reminder r);  // testing (serial "A")
void allDueNow();                 // testing (serial "AA"): every enabled reminder due now

// Reminder hours. allDay = any time (from/to/weekdays ignored). from/to are hours 0-23;
// from > to wraps past midnight.
struct Hours {
  bool allDay;
  int from, to;
  bool weekdays;
};
Hours hours();
void setHours(const Hours &h);
String hoursText(const Hours &h);  // "8 AM-6 PM"
String daysText(const Hours &h);   // "Weekdays", "Every day", "Any time"
bool activeNow();

}  // namespace reminders
