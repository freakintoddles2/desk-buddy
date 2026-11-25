#include "tz.h"

#include <ctype.h>
#include <string.h>

#include "daynum.h"
#include "store.h"

namespace tz {

const City CITIES[] = {
  {"New York", "EST5EDT,M3.2.0,M11.1.0", "America/New_York", "US", 40.71f, -74.01f},
  {"Chicago", "CST6CDT,M3.2.0,M11.1.0", "America/Chicago", "US", 41.88f, -87.63f},
  {"Denver", "MST7MDT,M3.2.0,M11.1.0", "America/Denver", "US", 39.74f, -104.99f},
  {"Phoenix", "MST7", "America/Phoenix", "US", 33.45f, -112.07f},
  {"Los Angeles", "PST8PDT,M3.2.0,M11.1.0", "America/Los_Angeles", "US", 34.05f, -118.24f},
  {"Honolulu", "HST10", "Pacific/Honolulu", "US", 21.31f, -157.86f},
  {"Toronto", "EST5EDT,M3.2.0,M11.1.0", "America/Toronto", "CA", 43.65f, -79.38f},
  {"Sao Paulo", "<-03>3", "America/Sao_Paulo", "BR", -23.55f, -46.63f},  // the built-in font has no "ã"
  {"London", "GMT0BST,M3.5.0/1,M10.5.0", "Europe/London", "GB", 51.51f, -0.13f},
  {"Berlin", "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Berlin", "DE", 52.52f, 13.40f},
  {"Dubai", "<+04>-4", "Asia/Dubai", "AE", 25.20f, 55.27f},
  {"Bengaluru", "IST-5:30", "Asia/Kolkata", "IN", 12.97f, 77.59f},
  {"Singapore", "<+08>-8", "Asia/Singapore", "SG", 1.35f, 103.82f},
  {"Tokyo", "JST-9", "Asia/Tokyo", "JP", 35.68f, 139.69f},
  {"Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Sydney", "AU", -33.87f, 151.21f},
  {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", "Pacific/Auckland", "NZ", -36.85f, 174.76f},
  // Added 2026-11-09 (international review).
  {"Vancouver", "PST8PDT,M3.2.0,M11.1.0", "America/Vancouver", "CA", 49.28f, -123.12f},
  {"Edmonton", "MST7MDT,M3.2.0,M11.1.0", "America/Edmonton", "CA", 53.55f, -113.49f},
  {"Winnipeg", "CST6CDT,M3.2.0,M11.1.0", "America/Winnipeg", "CA", 49.90f, -97.14f},
  {"Regina", "CST6", "America/Regina", "CA", 50.45f, -104.61f},
  {"Halifax", "AST4ADT,M3.2.0,M11.1.0", "America/Halifax", "CA", 44.65f, -63.58f},
  {"St. John's", "NST3:30NDT,M3.2.0,M11.1.0", "America/St_Johns", "CA", 47.56f, -52.71f},
  {"Anchorage", "AKST9AKDT,M3.2.0,M11.1.0", "America/Anchorage", "US", 61.22f, -149.90f},
  {"Mexico City", "CST6", "America/Mexico_City", "MX", 19.43f, -99.13f},
  {"Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4", "Europe/Athens", "GR", 37.98f, 23.73f},
  {"Perth", "AWST-8", "Australia/Perth", "AU", -31.95f, 115.86f},
  {"Adelaide", "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3", "Australia/Adelaide", "AU", -34.93f, 138.60f},
  {"Brisbane", "AEST-10", "Australia/Brisbane", "AU", -27.47f, 153.03f},
};
const int CITY_COUNT = sizeof(CITIES) / sizeof(CITIES[0]);

namespace {

struct Rule {
  int m = 0, w = 0, d = 0;
  int32_t secs = 7200;  // 02:00 local unless given
};

struct Zone {
  int32_t std = 0, dst = 0;  // seconds east of UTC
  bool hasDst = false;
  Rule start, end;
};

void skipName(const char *&p) {
  if (*p == '<') {
    while (*p && *p != '>') p++;
    if (*p) p++;
  } else {
    while (isalpha((unsigned char)*p)) p++;
  }
}

// [+-]hh[:mm[:ss]] -> seconds as written (POSIX: positive = west of UTC).
int32_t parseTime(const char *&p) {
  int sign = 1;
  if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
  int32_t v = strtol(p, (char **)&p, 10) * 3600;
  if (*p == ':') { p++; v += strtol(p, (char **)&p, 10) * 60; }
  if (*p == ':') { p++; v += strtol(p, (char **)&p, 10); }
  return sign * v;
}

bool parseRule(const char *&p, Rule &r) {
  if (*p != 'M') return false;  // only the Mm.w.d form is used by the presets
  p++;
  r.m = strtol(p, (char **)&p, 10);
  if (*p++ != '.') return false;
  r.w = strtol(p, (char **)&p, 10);
  if (*p++ != '.') return false;
  r.d = strtol(p, (char **)&p, 10);
  if (*p == '/') { p++; r.secs = parseTime(p); }
  return true;
}

Zone parse(const char *s) {
  Zone z;
  const char *p = s;
  skipName(p);
  z.std = -parseTime(p);
  if (!*p) return z;
  skipName(p);
  z.dst = z.std + 3600;
  if (*p && *p != ',') z.dst = -parseTime(p);
  if (*p == ',') {
    p++;
    if (parseRule(p, z.start) && *p == ',') {
      p++;
      z.hasDst = parseRule(p, z.end);
    }
  }
  return z;
}

// Day number of a rule's date in `year` ("the w-th weekday d of month m"; w 5 = last).
int32_t ruleDay(const Rule &r, int year) {
  int32_t first = daynum::fromYmd(year, r.m, 1);
  int32_t day = first + (r.d - daynum::weekday(first) + 7) % 7 + (r.w - 1) * 7;
  while (day >= first + daynum::daysInMonth(year, r.m)) day -= 7;
  return day;
}

int32_t offsetOf(const Zone &z, time_t t) {
  if (!z.hasDst) return z.std;
  int year = daynum::toYmd((int32_t)((t + z.std) / 86400)).y;
  // Start happens in standard time, end in daylight time.
  int64_t s = (int64_t)ruleDay(z.start, year) * 86400 + z.start.secs - z.std;
  int64_t e = (int64_t)ruleDay(z.end, year) * 86400 + z.end.secs - z.dst;
  bool dst = s < e ? (t >= s && t < e) : !(t >= e && t < s);  // southern hemisphere wraps the year
  return dst ? z.dst : z.std;
}

int devCity = PHOENIX;  // -1: the location's time
bool loaded = false;
int32_t locOff = 0;
bool locSaved = false;
String locName;
float locLat = 0, locLon = 0;
char locPosix[24] = "UTC0";

// Seconds east of UTC -> POSIX TZ ("<+07>-7", "<+0545>-5:45", "<-05>5").
void makeLocPosix() {
  int32_t a = abs(locOff);
  int h = a / 3600, m = a % 3600 / 60;
  char sign = locOff >= 0 ? '+' : '-';
  const char *posixSign = locOff >= 0 ? "-" : "";
  if (m) snprintf(locPosix, sizeof(locPosix), "<%c%02d%02d>%s%d:%02d", sign, h, m, posixSign, h, m);
  else snprintf(locPosix, sizeof(locPosix), "<%c%02d>%s%d", sign, h, posixSign, h);
}

void load() {
  if (loaded) return;
  loaded = true;
  locSaved = store::getStr("tzname").length() > 0;
  if (locSaved) {
    locName = store::getStr("tzname");
    locOff = constrain(store::getInt("tzoff", 0), -14 * 3600, 14 * 3600);
    locLat = store::getInt("tzlat", 0) / 10000.0f;
    locLon = store::getInt("tzlon", 0) / 10000.0f;
    makeLocPosix();
  }
  int c = store::getInt("tzcity", PHOENIX);
  devCity = c == -1 && locSaved ? -1 : constrain(c, 0, CITY_COUNT - 1);
}
String country;  // the weather location's, set on core 1
int8_t h24Cache = -1, dayFirstCache = -1;

void applyDevice() {
  setenv("TZ", devCity < 0 ? locPosix : CITIES[devCity].posix, 1);
  tzset();
}

}  // namespace

int32_t offset(const char *posix, time_t t) { return offsetOf(parse(posix), t); }

Local at(const char *posix, time_t t) {
  int64_t lt = (int64_t)t + offset(posix, t);
  int32_t day = (int32_t)(lt >= 0 ? lt / 86400 : (lt - 86399) / 86400);
  int32_t sec = (int32_t)(lt - (int64_t)day * 86400);
  daynum::Ymd a = daynum::toYmd(day);
  return {a.y, a.m, a.d, sec / 3600, sec / 60 % 60, sec % 60, day};
}


int deviceCity() {
  load();
  return devCity;
}

void setDeviceCity(int i) {
  load();
  devCity = constrain(i, 0, CITY_COUNT - 1);
  h24Cache = dayFirstCache = -1;  // the zone's country stands in until a weather location is known
  store::setInt("tzcity", devCity);
  applyDevice();
  Serial.printf("[tz] device zone %s (%s)\n", CITIES[devCity].name, CITIES[devCity].posix);
}

const char *devicePosix() { return deviceCity() < 0 ? locPosix : CITIES[devCity].posix; }

void fallbackCoords(float &lat, float &lon) {
  if (deviceCity() < 0) {
    lat = locLat;
    lon = locLon;
    return;
  }
  lat = CITIES[devCity].lat;
  lon = CITIES[devCity].lon;
}

bool hasLocationZone() { load(); return locSaved; }
int32_t locationOffset() { load(); return locOff; }
String locationZoneName() { load(); return locName; }

void setLocationZone(int32_t offset, const String &name, float lat, float lon, bool use) {
  load();
  bool changed = !locSaved || offset != locOff || name != locName;
  locSaved = true;
  locOff = constrain(offset, -14 * 3600, 14 * 3600);
  locName = name;
  locLat = lat;
  locLon = lon;
  makeLocPosix();
  if (changed) {
    store::setInt("tzoff", locOff);
    store::setStr("tzname", locName);
    store::setInt("tzlat", (int32_t)lroundf(lat * 10000));
    store::setInt("tzlon", (int32_t)lroundf(lon * 10000));
  }
  if (use) useLocationZone();
  else if (devCity < 0) applyDevice();
}

void useLocationZone() {
  load();
  if (!locSaved) return;
  bool was = devCity == -1;
  devCity = -1;
  h24Cache = dayFirstCache = -1;
  store::setInt("tzcity", -1);
  applyDevice();
  if (!was) Serial.printf("[tz] device zone: %s's time (%s)\n", locName.c_str(), locPosix);
}

void clearLocationZone() {
  load();
  if (!locSaved) return;
  locSaved = false;
  locName = "";
  store::remove("tzname");
  store::remove("tzoff");
  store::remove("tzlat");
  store::remove("tzlon");
  if (devCity < 0) setDeviceCity(PHOENIX);  // replaced by the preset right after
}

int cityFor(const char *iana, int32_t offsetNow) {
  if (!iana || !*iana) return -1;
  for (int i = 0; i < CITY_COUNT; i++)
    if (!strcmp(CITIES[i].iana, iana)) return i;
  // Zones that follow a preset's rules exactly.
  static const char *const SAME[][2] = {
    {"America/Detroit", "America/New_York"}, {"America/Indiana/", "America/New_York"},
    {"America/Kentucky/", "America/New_York"}, {"America/Boise", "America/Denver"},
    {"America/Montreal", "America/Toronto"}, {"America/Moncton", "America/Halifax"},
    {"America/Glace_Bay", "America/Halifax"}, {"America/Whitehorse", "America/Phoenix"},
    {"Europe/Dublin", "Europe/London"},
    {"Europe/Lisbon", "Europe/London"}, {"Atlantic/Canary", "Europe/London"},
    {"Australia/Melbourne", "Australia/Sydney"}, {"Australia/Hobart", "Australia/Sydney"},
    {"Australia/Canberra", "Australia/Sydney"}, {"Australia/Broken_Hill", "Australia/Adelaide"},
    {"Asia/Calcutta", "Asia/Kolkata"},
  };
  for (auto &m : SAME)
    if (!strncmp(iana, m[0], strlen(m[0])))
      for (int i = 0; i < CITY_COUNT; i++)
        if (!strcmp(CITIES[i].iana, m[1])) return i;
  // Otherwise, in Europe, a European preset with the same offset now: most of Europe shares
  // the EU rules. Elsewhere (Bogota looked like New York in November, an hour off all summer)
  // the caller follows the location's own offset instead.
  if (strncmp(iana, "Europe/", 7)) return -1;
  for (int i = 0; i < CITY_COUNT; i++)
    if (!strncmp(CITIES[i].iana, "Europe/", 7) && offset(CITIES[i].posix, time(nullptr)) == offsetNow) return i;
  return -1;
}

// ---------- regional formats ----------

namespace {
const char *countryNow() {
  if (country.length()) return country.c_str();
  return deviceCity() < 0 ? "" : CITIES[devCity].cc;
}

bool inList(const char *cc, const char *const *list) {
  for (; *list; list++)
    if (!strcmp(cc, *list)) return true;
  return false;
}
}  // namespace

ClockPref clockPref() { return (ClockPref)constrain(store::getInt("clock", CLOCK_AUTO), 0, 2); }

void setClockPref(ClockPref p) {
  store::setInt("clock", p);
  h24Cache = -1;
}

bool autoH24() {
  // Countries whose usual clock is 12-hour (CLDR's preferred hour cycle "h"); 24-hour elsewhere.
  static const char *const H12[] = {"US", "PR", "GU", "VI", "AS", "MP", "CA", "AU", "NZ", "IN", "PH",
                                    "PK", "BD", "EG", "SA", "MY", "CO", nullptr};
  return !inList(countryNow(), H12);
}

bool h24() {
  if (h24Cache < 0) {
    ClockPref p = clockPref();
    h24Cache = p == CLOCK_24 || (p == CLOCK_AUTO && autoH24());
  }
  return h24Cache;
}

bool dayFirst() {
  if (dayFirstCache < 0) {
    // Month first in the US (and territories), Canada and the Philippines; day first elsewhere.
    static const char *const MDY[] = {"US", "PR", "GU", "VI", "AS", "MP", "CA", "PH", "FM", "MH", nullptr};
    dayFirstCache = !inList(countryNow(), MDY);
  }
  return dayFirstCache;
}

void setCountry(const String &cc) {
  if (cc == country) return;
  country = cc;
  h24Cache = dayFirstCache = -1;
}

String timeText(int hour, int min) {
  char b[12];
  if (h24()) snprintf(b, sizeof(b), "%02d:%02d", hour, min);
  else snprintf(b, sizeof(b), "%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, min, hour < 12 ? "AM" : "PM");
  return String(b);
}

String clockText(int hour, int min) {
  char b[8];
  if (h24()) snprintf(b, sizeof(b), "%02d:%02d", hour, min);
  else snprintf(b, sizeof(b), "%d:%02d", hour % 12 == 0 ? 12 : hour % 12, min);
  return String(b);
}

String hourText(int hour) {
  char b[8];
  if (h24()) snprintf(b, sizeof(b), "%02d:00", hour);
  else snprintf(b, sizeof(b), "%d %s", hour % 12 == 0 ? 12 : hour % 12, hour < 12 ? "AM" : "PM");
  return String(b);
}

String dateText(const char *weekday, const char *month, int day) {
  char b[40];
  if (dayFirst()) snprintf(b, sizeof(b), "%s %d %s", weekday, day, month);
  else snprintf(b, sizeof(b), "%s, %s %d", weekday, month, day);
  return String(b);
}

}  // namespace tz
