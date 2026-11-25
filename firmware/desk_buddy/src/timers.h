// Focus sessions, labeled timers and the stopwatch (display.html "focus" and "timers and
// stopwatch"). Times run on millis(), so setting the clock doesn't disturb them. Running timers
// live in memory only: the board has no battery clock, so they can't survive a power cut anyway.
#pragma once

#include <Arduino.h>
#include <vector>

namespace timers {

struct Timer {
  uint32_t id;
  String label;
  uint32_t ms;       // total length
  uint32_t until;    // millis() when it ends (running)
  int32_t paused;    // ms left while paused, -1 when running
};

std::vector<Timer> &all();
Timer *find(uint32_t id);
uint32_t left(const Timer &t);
Timer &add(const String &label, int minutes);
void remove(uint32_t id);
void togglePause(Timer &t);
void addMinute(Timer &t);

// Focus
bool focusActive();
bool focusPaused();
bool focusRunning();     // active and not paused (reminders wait)
uint32_t focusLeft();
uint32_t focusLength();
int focusMinutes();      // the length the next session will have (5-60)
void setFocusMinutes(int m);
void startFocus();
void toggleFocusPause();
void stopFocus();
int sessionsToday();

// Stopwatch
bool swRunning();
uint32_t swElapsed();
const std::vector<uint32_t> &swLaps();
void swToggle();
bool swLap();            // false if not running
void swReset();

// Fires alerts for finished timers and focus sessions. Call from loop().
void tick();

// "4:05", "1:02:03"
String clockText(uint32_t ms);
// "4:05.37" (centis) or "4:05"
String swText(uint32_t ms, bool centis);

}  // namespace timers
