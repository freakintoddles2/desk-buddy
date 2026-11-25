// Auto brightness (BUILD_PLAN "Auto brightness"): the backlight follows the sun and a bedtime.
//   Morning  wake -> wake+1h          night min -> day max
//   Day                               day max (default 80%)
//   Dusk     sunset -30 -> +30 min    day max -> 30%
//   Evening                           30%
//   Bedtime  bed-1h -> bed            current -> night min
//   Night    bed -> wake              night min (default 5%, 1-25%)
// The level is worked out from the clock every second, never stepped phase to phase, so a
// reboot or a clock change lands on the right level. Ramps are in log space (each minute looks
// like the same change) with eased ends.
#pragma once

#include <Arduino.h>
#include <time.h>

namespace autodim {

enum Phase { MORNING, DAY, DUSK, EVENING, BEDTIME, NIGHT, PAUSED };
const char *phaseName(Phase p);

void begin();  // sets the backlight straight away (no fade), after store::begin()
void tick();   // from loop()

bool enabled();
void setEnabled(bool on);
int wakeMinutes();  // minutes after midnight, 30-min steps (default 360 = 6:00 AM)
void setWakeMinutes(int m);
int bedMinutes();   // default 1320 = 10:00 PM; may be after midnight (e.g. 30 = 12:30 AM)
void setBedMinutes(int m);
int dayMax();       // 1-100 % (default 80)
void setDayMax(int pct);
int nightMin();     // 1-25 % (default 5); never above day max
void setNightMin(int pct);
// Shows `pct` on the backlight while the Night min dial is open (call every frame; lapses
// about a second after the last call, so leaving the screen any way ends it).
void preview(int pct);

// The level the backlight shows now (auto, override or wake bump), 0-100 %.
float current();
Phase phase();
float targetAt(time_t t, Phase *ph = nullptr);  // the schedule alone

// The Brightness dial. Auto off: sets and saves the manual level. Auto on: overrides the
// schedule until the next phase change, then auto takes over again.
void setManual(int pct);
bool overriding();
time_t overrideEnds();  // when auto takes over again (0: not overriding, or no clock yet)

// Any knob, button or touch input. At night it raises the screen to 30% for 20 s.
void input();

// Serial testing: run the schedule's clock at `speed` x real time from `start` (0: now).
// speed <= 1 stops.
void fastForward(float speed, time_t start);
void printState();

}  // namespace autodim
