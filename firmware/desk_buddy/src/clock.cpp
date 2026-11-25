#include <esp_heap_caps.h>
#include <utility>
#include "clock.h"
#include "sounds.h"

#include <sys/time.h>

#include "app.h"
#include "autodim.h"
#include "board.h"
#include "buzzer.h"
#include "net.h"
#include "records.h"
#include "reminders.h"
#include "tray.h"
#include "tz.h"
#include "weather.h"

namespace clockx {
namespace {

char line[4096];  // big enough for a sound lab test sound (Z command)
size_t lineLen = 0;
volatile bool isTrusted = false;
bool wasTrusted = false;  // poll(): habits are re-anchored once when the clock becomes trusted

void setEpoch(time_t t) {
  timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
}

}  // namespace

void begin() {
  // The device's zone (Settings > Time zone, default Phoenix: MST all year).
  setenv("TZ", tz::devicePosix(), 1);
  tzset();
  if (time(nullptr) < 1700000000) {  // not set since power-up
    // __DATE__ "Nov  8 2026", __TIME__ "13:05:42", in the build machine's local time.
    struct tm t = {};
    strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &t);
    t.tm_isdst = -1;
    setEpoch(mktime(&t));
    Serial.printf("[clock] set from build time %s %s\n", __DATE__, __TIME__);
  }
}

void poll() {
  if (isTrusted && !wasTrusted) {
    wasTrusted = true;
    habits::rebase();  // marks made while the clock was wrong move to the right days
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[lineLen] = 0;
      // Test commands: S screenshot, F fetch weather now, K<n> knob steps, P press, H hold, W<+1|-1> swipe.
      if (lineLen == 1 && line[0] == 'S') {
        board::sendScreenshot();
      } else if (lineLen == 1 && line[0] == 'M') {
        // M: memory. Free now, the lowest it has been since boot, the largest single block.
        for (auto [name, cap] : {std::pair<const char *, uint32_t>{"internal", MALLOC_CAP_INTERNAL},
                                 {"psram", MALLOC_CAP_SPIRAM}})
          Serial.printf("[mem] %s total %u KB, free %u KB, lowest %u KB, largest block %u KB\n", name,
                        (unsigned)(heap_caps_get_total_size(cap) / 1024), (unsigned)(heap_caps_get_free_size(cap) / 1024),
                        (unsigned)(heap_caps_get_minimum_free_size(cap) / 1024),
                        (unsigned)(heap_caps_get_largest_free_block(cap) / 1024));
        Serial.printf("[mem] loop stack unused %u bytes of 16 KB\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
      } else if (lineLen == 2 && line[0] == 'C') {
        tz::setClockPref((tz::ClockPref)constrain(line[1] - '0', 0, 2));  // C0 auto, C1 12-hour, C2 24-hour
      } else if (lineLen == 2 && line[0] == 'B' && line[1] == '?') {
        autodim::printState();
      } else if (lineLen > 2 && line[0] == 'B' && line[1] == 'x') {
        // Bx<speed>[,HHMM]: run auto brightness's clock fast (from HH:MM today); Bx0 stops.
        float sp = atof(line + 2);
        time_t start = 0;
        if (const char *c = strchr(line, ',')) {
          int hhmm = atoi(c + 1);
          struct tm l = local();
          l.tm_hour = hhmm / 100; l.tm_min = hhmm % 100; l.tm_sec = 0;
          start = mktime(&l);
        }
        autodim::fastForward(sp, start);
      } else if (lineLen > 2 && line[0] == 'B' && line[1] == 'R') {
        uint32_t d = strtoul(line + 2, nullptr, 10);  // BR<duty>: raw backlight duty 0-4095
        board::setBacklightDuty(d);
        Serial.printf("[bright] raw duty %lu/4095 (%.2f%%)\n", (unsigned long)d, d * 100.0f / 4095);
      } else if (lineLen >= 4 && !strncmp(line, "SND", 3)) {
        sounds::serialCommand(line + 3);  // the sound lab: read and set sounds
      } else if (line[0] == 'Z') {
        buzzer::playCommand(line + 1);  // sound lab: play a sound on the buzzer
      } else if (lineLen == 1 && line[0] == 'N') {
        net::testDrop();
      } else if (lineLen == 1 && line[0] == 'F') {
        weather::refresh();
      } else if (lineLen == 1 && (line[0] == 'U' || line[0] == 'V')) {
        app::injectSwipeVertical(line[0] == 'U' ? -1 : 1, line[0] == 'U' ? 470 : 240);  // swipe up from the bottom / down
      } else if (lineLen == 1 && line[0] == 'Q') {
        tray::testQuick();  // tap the tray's brightness button
      } else if (lineLen == 1 && line[0] == 'D') {
        tray::testToggle();  // open/close the fast travel tray
      } else if (line[0] == 'Y') {
        tray::testFlick(lineLen > 1 ? atoi(line + 1) : 2);  // scripted pull + flick
      } else if (line[0] == 'L' && lineLen > 1) {
        // L<city>: weather search (results print when done); L#<n>: choose result n.
        if (line[1] == '#') {
          auto r = weather::searchResults();
          int n = atoi(line + 2);
          if (n >= 0 && n < (int)r.size()) weather::choose(r[n]);
        } else {
          weather::search(String(line + 1));
        }
      } else if (lineLen == 2 && line[0] == 'A' && line[1] == 'A') {
        reminders::allDueNow();
      } else if (lineLen == 1 && line[0] == 'A') {
        reminders::fireNow(store::WATER);
      } else if (line[0] == 'K') {
        board::injectKnob(atoi(line + 1));
      } else if (lineLen == 1 && line[0] == 'P') {
        board::injectButton(board::Button::Press);
      } else if (lineLen == 1 && line[0] == 'H') {
        board::injectButton(board::Button::LongPress);
      } else if (lineLen == 1 && line[0] == 'G') {
        board::injectButton(board::Button::HoldHome);  // the 3 s hold
      } else if (line[0] == 'W') {
        app::injectSwipe(atoi(line + 1) >= 0 ? 1 : -1);
      } else if (lineLen > 1 && line[0] == 'T') {
        time_t t = (time_t)strtoll(line + 1, nullptr, 10);
        if (t > 1700000000) {
          setEpoch(t);
          isTrusted = true;
          struct tm l = local();
          Serial.printf("[clock] set to %04d-%02d-%02d %02d:%02d:%02d\n", l.tm_year + 1900, l.tm_mon + 1,
                        l.tm_mday, l.tm_hour, l.tm_min, l.tm_sec);
        }
      }
      lineLen = 0;
    } else if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }
}

struct tm local() {
  time_t t = time(nullptr);
  struct tm l;
  localtime_r(&t, &l);
  return l;
}

float hourOfDay() {
  struct tm l = local();
  return l.tm_hour + l.tm_min / 60.0f + l.tm_sec / 3600.0f;
}

void setLocal(int year, int month, int day, int hour, int minute) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = local().tm_sec;
  t.tm_isdst = -1;
  setEpoch(mktime(&t));
  isTrusted = true;
  Serial.printf("[clock] set by hand to %04d-%02d-%02d %02d:%02d\n", year, month, day, hour, minute);
}

bool trusted() { return isTrusted; }
void markTrusted() { isTrusted = true; }

void setFromNetwork(time_t t, const char *source) {
  setEpoch(t);
  isTrusted = true;
  Serial.printf("[clock] set from %s\n", source);
}

float utcOffsetHours() { return tz::offset(tz::devicePosix(), time(nullptr)) / 3600.0f; }

}  // namespace clockx
