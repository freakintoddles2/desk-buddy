// Habits and countdowns (display.html db.habits / db.countdowns), kept in flash so they
// survive a reboot or power loss.
#pragma once

#include <Arduino.h>
#include <bitset>
#include <vector>

namespace habits {

constexpr int MAX = 12;
constexpr int HISTORY = 384;  // days of check-offs kept per habit (streaks cap here)

struct Habit {
  uint32_t id;
  String name;
  int32_t anchor;              // day number of bit 0
  std::bitset<HISTORY> done;   // bit i = day (anchor - i)
};

void begin();
// The clock has just been set (network or by hand): re-anchor histories left in the future
// by a wrong clock, so check-offs land on the right days again.
void rebase();
const std::vector<Habit> &all();
Habit *find(uint32_t id);
uint32_t add(const String &name);  // 0 if the list is full
void remove(uint32_t id);

bool isDone(const Habit &h, int32_t day);
void toggle(uint32_t id, int32_t day);
int streak(const Habit &h);  // days in a row, counting today if done, else up to yesterday
int doneInLast7(const Habit &h);
int doneToday();             // how many habits are done today

}  // namespace habits

namespace countdowns {

constexpr int MAX = 20;

struct Countdown {
  uint32_t id;
  String name;
  int32_t day;  // day number of the date
};

void begin();
const std::vector<Countdown> &all();
Countdown *find(uint32_t id);
uint32_t add(const String &name, int32_t day);  // 0 if the list is full
void remove(uint32_t id);
void rename(uint32_t id, const String &name);
void setDay(uint32_t id, int32_t day);

int32_t daysUntil(const Countdown &c);  // negative once it has passed
// Upcoming first (soonest first), then past ones (most recent first).
std::vector<Countdown> sorted();
int upcomingCount();
// "Today", "Tomorrow", "12 days", "3d ago".
String badge(int32_t n);

}  // namespace countdowns
