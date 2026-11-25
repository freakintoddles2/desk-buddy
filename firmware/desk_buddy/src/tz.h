// Time zones from built-in POSIX TZ rules ("EST5EDT,M3.2.0,M11.1.0"), daylight saving
// included, for World's cities and the device's own zone. Evaluated here rather than by
// switching the C library's TZ, so the weather task and the UI never see a borrowed zone.
// Rules are current as of 2026; a country changing its rules needs a firmware update.
// Also the regional formats: 12- or 24-hour clock and day-first or month-first dates, by
// country (the weather location's, else the device zone's city).
#pragma once

#include <Arduino.h>
#include <time.h>

namespace tz {

struct City {
  const char *name;
  const char *posix;
  const char *iana;  // the IANA zone it stands for (the weather API and phones name zones this way)
  const char *cc;    // country code, for the formats before a weather location is known
  float lat, lon;    // where the sun and moon are before a weather location is known
};

// Preset cities (display.html CITY_PRESETS). Stored by index, so only append to this list.
extern const City CITIES[];
extern const int CITY_COUNT;
constexpr int PHOENIX = 3;  // the default device zone

// Seconds east of UTC in effect at `t` for a POSIX TZ string.
int32_t offset(const char *posix, time_t t);

struct Local {
  int y, m, d, hour, min, sec;  // m 1-12, hour 0-23
  int32_t day;                  // day number (daynum.h)
};
Local at(const char *posix, time_t t);

// The device's own zone (Settings > Time zone): a CITIES index, saved in NVS, or -1 when it
// follows the weather location's own UTC offset (a place with no preset, e.g. Bangkok).
int deviceCity();
void setDeviceCity(int i);  // also re-applies the C library's TZ
const char *devicePosix();
// The preset for an IANA zone ("Pacific/Auckland"), or -1. In Europe, where the presets' EU
// rules are shared, offsetNow (seconds east of UTC now) picks a preset with the same offset.
// Elsewhere a zone without a preset gets -1: its daylight saving can't be guessed from today.
int cityFor(const char *iana, int32_t offsetNow);

// The location's time: a fixed UTC offset taken from each forecast (so daylight saving is
// followed within a refresh while online). Saved with the place's name and position.
bool hasLocationZone();          // one is saved (the location has no preset)
int32_t locationOffset();        // seconds east of UTC
String locationZoneName();       // "Bangkok"
// Save it; `use` also makes it the device's zone.
void setLocationZone(int32_t offset, const String &name, float lat, float lon, bool use);
void useLocationZone();          // the device follows the saved one (Settings > Time zone)
void clearLocationZone();        // the location has a preset after all
// The device zone's city position (sun and moon before the weather location is known).
void fallbackCoords(float &lat, float &lon);

// ---------- regional formats ----------

enum ClockPref : uint8_t { CLOCK_AUTO, CLOCK_12, CLOCK_24 };
ClockPref clockPref();  // Settings > Time format
void setClockPref(ClockPref p);
bool autoH24();         // what Auto means here
bool h24();             // 24-hour clock
bool dayFirst();        // "Monday 9 Nov" rather than "Monday, Nov 9"
// The weather location's country code (core 1 only; "" = none yet).
void setCountry(const String &cc);

String timeText(int hour, int min);   // "4:05 PM" or "16:05"
String clockText(int hour, int min);  // Home's big clock: "4:05" or "16:05"
String hourText(int hour);            // "4 PM" or "16:00"
// "Monday, Nov 9" or "Monday 9 Nov" (day and month names as given).
String dateText(const char *weekday, const char *month, int day);

}  // namespace tz
