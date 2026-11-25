// Weather from Open-Meteo (free, no API key, no account), over plain HTTP (see weather.cpp). Only runs once Wi-Fi is set up.
// The location is either typed on the phone ("Phoenix, AZ", "Wellington, NZ", "Toronto, ON",
// a US ZIP) and looked up with Open-Meteo's geocoding API, or picked on the device from a
// search (Settings > Weather), which stores the place's coordinates. The forecast is fetched
// every 30 minutes on a background task so the UI never waits on the network.
// Units: Auto (°F + mph in the US, °C + km/h elsewhere), or fixed in Settings > Weather.
// A newly chosen location also sets the device's time zone (the closest preset) and the
// country that the clock and date formats follow (tz.h).
#pragma once

#include <Arduino.h>
#include <time.h>
#include <vector>

namespace weather {

// Condition groups (from the WMO weather codes Open-Meteo returns); each has an icon.
enum Cond : uint8_t { CLEAR, PARTLY, CLOUDY, FOG, RAIN, SNOW, STORM };
Cond condOf(int wmoCode);
const char *condName(int wmoCode);  // "Clear", "Partly cloudy", "Light rain", ...

// A humidity, wind or rain chance the forecast didn't include (shown as missing, not as 0).
constexpr uint8_t NO_VALUE = 255;

struct Hour {
  time_t at;
  int16_t temp;
  uint8_t code, rain;  // WMO code, chance of rain %
  bool day;
};

struct Day {
  time_t date;  // local midnight at the location
  int16_t hi, lo;
  uint8_t code, rain;
};

struct Current {
  int16_t temp, feels;
  uint8_t code, humidity, wind;
  const char *windDir;  // "" when the forecast has no direction
  bool day;
};

struct Data {
  time_t fetched = 0;  // 0 = no data yet
  String place;        // "Phoenix" as found by the geocoder
  bool celsius = false;
  float lat = 0, lon = 0;
  String cc;            // the place's country code ("NZ"), when known
  String zone;          // its IANA time zone ("Pacific/Auckland")
  int32_t utcOffset = 0;  // its seconds east of UTC at fetch time
  Current now{};
  std::vector<Hour> hours;  // from the fetch hour on; use upcomingHours()
  std::vector<Day> days;    // from the fetch day on; use upcomingDays()

  // Hours/days that haven't passed yet (the data can be up to 30 minutes old, or older offline).
  int firstHour() const;
  int firstDay() const;
};

enum class State { Off, Waiting, Loading, Ready, NotFound, Failed };

void begin();
void tick();  // call from loop()

State state();
// Copy of the latest data. Returns false if there is none yet.
bool get(Data &out);
// Increments whenever new data arrives (cheap check for "redraw").
uint32_t version();
// Fetch again now (serial "F", or after the location changes).
void refresh();
// Coordinates of the weather location, if it has been found (Home's sun ring uses them).
bool coords(float &lat, float &lon);

// ---------- Settings > Weather ----------

// A place from a search. Names are folded to ASCII (the built-in font has no accents).
struct Place {
  String name, region, country, cc;
  float lat = 0, lon = 0;
};
enum class Search { Idle, Busy, Done, Failed };
// Look `query` (a city name) up in the background; poll searchState().
void search(const String &query);
Search searchState();
std::vector<Place> searchResults();
// Make `p` the weather location: its forecast is fetched first, and only once that works is
// the place saved and shown. If it can't be fetched (offline, server trouble), the previous
// location stays. Poll chooseState() for the result.
void choose(const Place &p);
enum class Choose { Idle, Busy, Done, Failed };
Choose chooseState();
// "Wellington, New Zealand" for a picked place; what was typed otherwise.
String locationLabel();

enum Units : uint8_t { UNITS_AUTO, UNITS_F, UNITS_C };
Units units();
void setUnits(Units u);
// What Auto means for the current location (true = °C).
bool autoCelsius();

// "Montréal" -> "Montreal" (Latin-1 and Latin Extended-A letters to their base letter).
String ascii(const String &utf8);

}  // namespace weather
