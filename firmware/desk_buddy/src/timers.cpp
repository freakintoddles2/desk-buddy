#include "timers.h"

#include "app.h"
#include "clock.h"
#include "modes.h"
#include "store.h"
#include "sounds.h"

namespace timers {
namespace {

std::vector<Timer> list;
uint32_t nextId = 1;

struct Focus {
  bool active = false;
  uint32_t ms = 0, until = 0;
  int32_t paused = -1;
} focus;

struct Stopwatch {
  bool running = false;
  uint32_t start = 0, acc = 0;
  std::vector<uint32_t> laps;
} sw;

int todayKey() {
  struct tm t = clockx::local();
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

void logSession() {
  int day = todayKey();
  int n = store::getInt("focusDay", 0) == day ? store::getInt("focusN", 0) : 0;
  store::setInt("focusDay", day);
  store::setInt("focusN", n + 1);
}

bool due(uint32_t until) { return (int32_t)(millis() - until) >= 0; }

}  // namespace

std::vector<Timer> &all() { return list; }

Timer *find(uint32_t id) {
  for (Timer &t : list)
    if (t.id == id) return &t;
  return nullptr;
}

uint32_t left(const Timer &t) {
  if (t.paused >= 0) return t.paused;
  return due(t.until) ? 0 : t.until - millis();
}

Timer &add(const String &label, int minutes) {
  uint32_t ms = minutes * 60000UL;
  list.push_back({nextId++, label, ms, millis() + ms, -1});
  Serial.printf("[timer] %s %d min\n", label.c_str(), minutes);
  return list.back();
}

void remove(uint32_t id) {
  for (size_t i = 0; i < list.size(); i++)
    if (list[i].id == id) { list.erase(list.begin() + i); return; }
}

void togglePause(Timer &t) {
  if (t.paused >= 0) { t.until = millis() + t.paused; t.paused = -1; }
  else t.paused = left(t);
}

void addMinute(Timer &t) {
  if (t.paused >= 0) t.paused += 60000; else t.until += 60000;
  t.ms += 60000;
}

bool focusActive() { return focus.active; }
bool focusPaused() { return focus.active && focus.paused >= 0; }
bool focusRunning() { return focus.active && focus.paused < 0; }
uint32_t focusLength() { return focus.ms; }

uint32_t focusLeft() {
  if (!focus.active) return 0;
  if (focus.paused >= 0) return focus.paused;
  return due(focus.until) ? 0 : focus.until - millis();
}

int focusMinutes() { return store::getInt("focusMin", 25); }
void setFocusMinutes(int m) { store::setIntLater("focusMin", constrain(m, 1, 60)); }

void startFocus() {
  sounds::play(sounds::FOCUS_START);
  focus.active = true;
  focus.ms = focusMinutes() * 60000UL;
  focus.until = millis() + focus.ms;
  focus.paused = -1;
  Serial.printf("[focus] start %d min\n", focusMinutes());
}

void toggleFocusPause() {
  if (!focus.active) return;
  if (focus.paused >= 0) { focus.until = millis() + focus.paused; focus.paused = -1; }
  else focus.paused = focusLeft();
}

void stopFocus() { focus.active = false; }

int sessionsToday() { return store::getInt("focusDay", 0) == todayKey() ? store::getInt("focusN", 0) : 0; }

bool swRunning() { return sw.running; }
uint32_t swElapsed() { return sw.acc + (sw.running ? millis() - sw.start : 0); }
const std::vector<uint32_t> &swLaps() { return sw.laps; }

void swToggle() {
  if (sw.running) { sw.acc += millis() - sw.start; sw.running = false; }
  else { sw.start = millis(); sw.running = true; }
}

bool swLap() {
  if (!sw.running) return false;
  uint32_t sum = 0;
  for (uint32_t l : sw.laps) sum += l;
  sw.laps.push_back(swElapsed() - sum);
  return true;
}

void swReset() { sw = Stopwatch(); }

void tick() {
  if (focus.active && focus.paused < 0 && due(focus.until)) {
    int min = focus.ms / 60000;
    focus.active = false;
    logSession();
    int n = sessionsToday();
    String sub = String(min) + " min done \xE2\x80\xA2 " + n + (n == 1 ? " session" : " sessions") + " today";
    Serial.println("[focus] done");
    sounds::play(sounds::FOCUS_DONE);
    app::pushAlert(new AlertMode("Focus", "Nice work", sub, {
      {"Done", ui::Tone::Primary, nullptr},
      {"5 min break", ui::Tone::Normal, []() { add("Break", 5); }},
    }));
  }
  for (size_t i = 0; i < list.size();) {
    Timer t = list[i];
    if (t.paused < 0 && due(t.until)) {
      list.erase(list.begin() + i);
      Serial.printf("[timer] %s done\n", t.label.c_str());
      sounds::play(sounds::TIMER_DONE);
      String label = t.label;
      app::pushAlert(new AlertMode("Timer", label, "Time's up", {
        {"Dismiss", ui::Tone::Primary, nullptr},
        {"+5 min", ui::Tone::Normal, [label]() { add(label, 5); }},
      }));
    } else {
      i++;
    }
  }
}

String clockText(uint32_t ms) {
  uint32_t s = (ms + 999) / 1000;  // count up to the next whole second, like display.html
  char b[16];
  if (s >= 3600) snprintf(b, sizeof(b), "%lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
  else snprintf(b, sizeof(b), "%lu:%02lu", s / 60, s % 60);
  return b;
}

String swText(uint32_t ms, bool centis) {
  uint32_t s = ms / 1000, cs = ms / 10 % 100;
  char b[20];
  if (s >= 3600) snprintf(b, sizeof(b), "%lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
  else snprintf(b, sizeof(b), "%lu:%02lu", s / 60, s % 60);
  String out = b;
  if (centis) {
    snprintf(b, sizeof(b), ".%02lu", cs);
    out += b;
  }
  return out;
}

}  // namespace timers
