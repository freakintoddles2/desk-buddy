#include "weather.h"

#include <algorithm>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "clock.h"
#include "daynum.h"
#include "net.h"
#include "store.h"
#include "tz.h"

namespace weather {
namespace {

constexpr uint32_t REFRESH_MS = 30 * 60 * 1000UL;
// After a failed fetch: try again after 1, 2, 5, 10, 10, ... minutes.
constexpr uint32_t RETRY_MS[] = {60000, 120000, 300000, 600000};

SemaphoreHandle_t mtx = nullptr;
Data data;               // guarded by mtx
String wantLoc;          // guarded by mtx; written by tick(), read by the task
volatile State st = State::Off;
volatile uint32_t ver = 0;
volatile bool force = false;
volatile bool enabled = false;  // Wi-Fi set up
volatile bool foundCelsius = false;  // Auto units for the last place found
// Settings > Weather > Units, read every second by the task on core 0: kept here so it never
// reads the settings store while the UI writes it.
volatile int unitsNow = UNITS_AUTO;
uint32_t appliedVer = 0;  // tick(): the data the zone and country were last taken from
// Network time may set the clock from a forecast's Date header (Settings > Date & time on).
volatile bool httpTimeOk = true;

// Settings > Weather > Location: the place being tried. The task fetches its forecast; only
// then does tick() save it on core 1 (the store is written on core 1 only).
volatile Choose chooseSt = Choose::Idle;
String tryLoc;             // guarded by mtx
bool commitLoc = false;    // guarded by mtx: tryLoc worked, save it
String seenLoc;            // tick(): the saved location last seen (core 1 only)

// Search (Settings > Weather): one background lookup at a time.
volatile Search searchSt = Search::Idle;
std::vector<Place> found;  // guarded by mtx
String searchQ;            // guarded by mtx

struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

// ---------- HTTP ----------

String urlEncode(const String &s) {
  String o;
  const char *hex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

constexpr uint32_t BODY_DEADLINE_MS = 15000;  // the whole body, however slowly it trickles in
constexpr size_t BODY_MAX = 64 * 1024;        // a forecast is ~2 KB, a search ~3 KB

// GET `url` into `body`. Reads the body itself instead of HTTPClient::getString(), which
// busy-waits on a stalled connection: on core 0 that starved the idle task and the watchdog
// rebooted the device (a slow server meant a reboot loop). HTTP/1.0 keeps the body unchunked
// and ends it by closing the connection. `date` gets the response's Date header, if asked.
bool httpGet(const String &url, String &body, String *date = nullptr) {
  // Plain HTTP: this Arduino core build has HTTPS compiled out (no NetworkClientSecure, and
  // ESP-IDF's HTTP client has HTTPS disabled). Only the location and the public forecast cross
  // the network; there are no keys or accounts involved.
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setUserAgent("DeskBuddy");
  http.useHTTP10(true);
  const char *keep[] = {"Date"};
  http.collectHeaders(keep, 1);
  body = "";
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[weather] HTTP %d for [%s]\n", code, url.c_str());
    http.end();
    return false;
  }
  if (date) *date = http.header("Date");
  WiFiClient *in = http.getStreamPtr();
  int len = http.getSize();  // -1: until the server closes
  body.reserve(len > 0 && len < (int)BODY_MAX ? len + 1 : 4096);
  uint8_t buf[512];
  uint32_t t0 = millis();
  bool ok = in != nullptr;
  while (ok && (len < 0 || (int)body.length() < len)) {
    if (millis() - t0 > BODY_DEADLINE_MS) {
      Serial.printf("[weather] body timed out after %u bytes\n", body.length());
      ok = false;
      break;
    }
    int n = in->available();
    if (n > 0) {
      int r = in->read(buf, min(n, (int)sizeof(buf)));
      if (r > 0) body.concat((const char *)buf, r);
      if (body.length() > BODY_MAX) {
        Serial.println("[weather] body too large");
        ok = false;
      }
    } else if (!in->connected()) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  if (ok && len >= 0 && (int)body.length() < len) {
    Serial.printf("[weather] body cut off: %u of %d bytes\n", body.length(), len);
    ok = false;
  }
  http.end();
  if (!ok) body = "";
  return ok;
}

// "Tue, 10 Nov 2026 02:09:30 GMT" -> unix time, or 0.
time_t parseHttpDate(const String &d) {
  static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char mon[4] = {0};
  int day, year, hh, mm, ss;
  if (sscanf(d.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return 0;
  for (int m = 0; m < 12; m++)
    if (!strcmp(mon, MON[m])) return (time_t)daynum::fromYmd(year, m + 1, day) * 86400 + hh * 3600 + mm * 60 + ss;
  return 0;
}

// No network time yet (blocked or slow): a working forecast's Date header sets the clock.
void maybeSetClock(const String &date) {
  if (clockx::trusted() || !httpTimeOk) return;
  time_t t = parseHttpDate(date);
  if (t > 1700000000) clockx::setFromNetwork(t, "forecast Date header");
}

// ---------- location lookup ----------

const char *const STATES[][2] = {
  {"AL", "Alabama"}, {"AK", "Alaska"}, {"AZ", "Arizona"}, {"AR", "Arkansas"}, {"CA", "California"},
  {"CO", "Colorado"}, {"CT", "Connecticut"}, {"DE", "Delaware"}, {"DC", "District of Columbia"},
  {"FL", "Florida"}, {"GA", "Georgia"}, {"HI", "Hawaii"}, {"ID", "Idaho"}, {"IL", "Illinois"},
  {"IN", "Indiana"}, {"IA", "Iowa"}, {"KS", "Kansas"}, {"KY", "Kentucky"}, {"LA", "Louisiana"},
  {"ME", "Maine"}, {"MD", "Maryland"}, {"MA", "Massachusetts"}, {"MI", "Michigan"}, {"MN", "Minnesota"},
  {"MS", "Mississippi"}, {"MO", "Missouri"}, {"MT", "Montana"}, {"NE", "Nebraska"}, {"NV", "Nevada"},
  {"NH", "New Hampshire"}, {"NJ", "New Jersey"}, {"NM", "New Mexico"}, {"NY", "New York"},
  {"NC", "North Carolina"}, {"ND", "North Dakota"}, {"OH", "Ohio"}, {"OK", "Oklahoma"}, {"OR", "Oregon"},
  {"PA", "Pennsylvania"}, {"RI", "Rhode Island"}, {"SC", "South Carolina"}, {"SD", "South Dakota"},
  {"TN", "Tennessee"}, {"TX", "Texas"}, {"UT", "Utah"}, {"VT", "Vermont"}, {"VA", "Virginia"},
  {"WA", "Washington"}, {"WV", "West Virginia"}, {"WI", "Wisconsin"}, {"WY", "Wyoming"},
};

// Canadian provinces and Australian states, so "Toronto, ON" or "Perth, WA" style
// qualifiers work too (WA also means Washington; whichever the geocoder ranks first wins).
const char *const REGIONS[][2] = {
  {"ON", "Ontario"}, {"QC", "Quebec"}, {"BC", "British Columbia"}, {"AB", "Alberta"},
  {"MB", "Manitoba"}, {"SK", "Saskatchewan"}, {"NS", "Nova Scotia"}, {"NB", "New Brunswick"},
  {"NL", "Newfoundland and Labrador"}, {"PE", "Prince Edward Island"}, {"YT", "Yukon"},
  {"NT", "Northwest Territories"}, {"NU", "Nunavut"},
  {"NSW", "New South Wales"}, {"VIC", "Victoria"}, {"QLD", "Queensland"}, {"SA", "South Australia"},
  {"WA", "Western Australia"}, {"TAS", "Tasmania"}, {"ACT", "Australian Capital Territory"},
  {"UK", "United Kingdom"},
};

// Countries that use °F.
bool fahrenheitCountry(const String &cc) { return cc == "US" || cc == "LR" || cc == "MM" || cc == "KY" || cc == "BS"; }

bool qualMatches(const String &qual, const String &a1, const String &co, const String &cc) {
  if (a1.equalsIgnoreCase(qual) || co.equalsIgnoreCase(qual) || cc.equalsIgnoreCase(qual)) return true;
  for (auto &s : STATES)
    if (qual.equalsIgnoreCase(s[0]) && a1.equalsIgnoreCase(s[1])) return true;
  for (auto &s : REGIONS)
    if (qual.equalsIgnoreCase(s[0]) && (a1.equalsIgnoreCase(s[1]) || co.equalsIgnoreCase(s[1]))) return true;
  if (qual.equalsIgnoreCase("USA") && cc == "US") return true;
  return false;
}

// "Mesa, AZ" -> "Mesa" + "AZ". Without a comma (the device's letter ring has none, and people
// type it that way on phones too) a last word that is a known state/province/country code is
// taken as the qualifier: "MESA AZ" searched whole finds "Mesa Azur" or nothing. `guessed`
// says the split came from a space, so callers can fall back to the whole text.
bool knownCode(const String &w) {
  for (auto &s : STATES) if (w.equalsIgnoreCase(s[0])) return true;
  for (auto &s : REGIONS) if (w.equalsIgnoreCase(s[0])) return true;
  return w.length() == 2 || w.equalsIgnoreCase("USA");  // any 2 letters may be a country code (NZ, CA, GB...)
}
void splitQuery(const String &in, String &name, String &qual, bool &guessed) {
  name = in;
  qual = "";
  guessed = false;
  int comma = in.indexOf(',');
  if (comma >= 0) {
    name = in.substring(0, comma);
    qual = in.substring(comma + 1);
  } else {
    String t = in;
    t.trim();
    int sp = t.lastIndexOf(' ');
    if (sp > 0 && knownCode(t.substring(sp + 1))) {
      name = t.substring(0, sp);
      qual = t.substring(sp + 1);
      guessed = true;
    }
  }
  name.trim();
  qual.trim();
}

enum class Geo { Found, NotFound, Error };

// "Phoenix, AZ" -> name "Phoenix", qualifier "AZ" (a state, state code, country or country
// code narrows the matches). A ZIP or plain name takes the first (most populous) match.
Geo geocode(const String &loc, float &lat, float &lon, String &place, String &cc) {
  // A place picked on the device: "@lat,lon|Name|Region, Country|CC".
  if (loc.startsWith("@")) {
    int p1 = loc.indexOf('|'), p2 = loc.indexOf('|', p1 + 1), p3 = loc.indexOf('|', p2 + 1);
    int comma = loc.indexOf(',');
    if (p1 < 0 || comma < 0 || comma > p1) return Geo::NotFound;
    lat = loc.substring(1, comma).toFloat();
    lon = loc.substring(comma + 1, p1).toFloat();
    place = loc.substring(p1 + 1, p2 < 0 ? loc.length() : p2);
    cc = p3 < 0 ? String("") : loc.substring(p3 + 1);
    return Geo::Found;
  }
  String name, qual;
  bool guessed;
  splitQuery(loc, name, qual, guessed);
  if (!name.length()) return Geo::NotFound;

  String body;
  if (!httpGet("http://geocoding-api.open-meteo.com/v1/search?count=10&language=en&format=json&name=" + urlEncode(name), body))
    return Geo::Error;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return Geo::Error;
  JsonArray res = doc["results"].as<JsonArray>();
  if (res.isNull() || res.size() == 0) return Geo::NotFound;
  JsonObject pick;
  if (qual.length()) {
    for (JsonObject r : res) {
      String a1 = r["admin1"] | "", co = r["country"] | "", rcc = r["country_code"] | "";
      if (qualMatches(qual, ascii(a1), ascii(co), rcc)) {
        pick = r;
        break;
      }
    }
    if (pick.isNull()) {
      if (!guessed) return Geo::NotFound;
      // The last word wasn't a qualifier after all: look the whole text up.
      String all = loc;
      all.trim();
      if (!httpGet("http://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" + urlEncode(all), body))
        return Geo::Error;
      if (deserializeJson(doc, body)) return Geo::Error;
      res = doc["results"].as<JsonArray>();
      if (res.isNull() || res.size() == 0) return Geo::NotFound;
      pick = res[0];
    }
  } else {
    pick = res[0];
  }
  lat = pick["latitude"] | 0.0f;
  lon = pick["longitude"] | 0.0f;
  place = ascii((const char *)(pick["name"] | ""));
  cc = (const char *)(pick["country_code"] | "");
  Serial.printf("[weather] %s -> %s, %s (%.3f, %.3f)\n", loc.c_str(), place.c_str(),
                (const char *)(pick["admin1"] | ""), lat, lon);
  return Geo::Found;
}

// ---------- forecast ----------

const char *windDir(float deg) {
  static const char *const D[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return D[(int)((fmodf(deg, 360) + 382.5f) / 45) % 8];
}

// Missing (null) values are never shown as 0: the current temperature and conditions are
// required, hours and days end at the first one without a time, temperature or code, and a
// missing humidity, wind or rain chance is NO_VALUE.
bool fetchForecast(float lat, float lon, bool celsius, Data &out, String &date) {
  char url[560];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m,is_day"
           "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&temperature_unit=%s&wind_speed_unit=%s&timezone=auto&timeformat=unixtime"
           "&forecast_days=8&forecast_hours=30",
           lat, lon, celsius ? "celsius" : "fahrenheit", celsius ? "kmh" : "mph");
  out.celsius = celsius;
  String body;
  if (!httpGet(url, body, &date)) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[weather] bad JSON: %s\n", err.c_str());
    return false;
  }
  JsonObject c = doc["current"];
  if (c.isNull() || c["temperature_2m"].isNull() || c["weather_code"].isNull()) {
    Serial.println("[weather] forecast has no current conditions");
    return false;
  }
  // When the reading is from (the server's clock), so its age is right even if this device's
  // clock was still wrong when it arrived.
  long at = c["time"] | 0L;
  out.fetched = at > 1700000000 ? (time_t)at : 0;
  out.zone = (const char *)(doc["timezone"] | "");
  out.utcOffset = doc["utc_offset_seconds"] | 0;
  out.now.temp = (int16_t)lroundf(c["temperature_2m"].as<float>());
  out.now.feels = c["apparent_temperature"].isNull() ? out.now.temp : (int16_t)lroundf(c["apparent_temperature"].as<float>());
  out.now.humidity = c["relative_humidity_2m"].isNull() ? NO_VALUE : (uint8_t)(c["relative_humidity_2m"].as<int>());
  out.now.code = c["weather_code"].as<int>();
  out.now.wind = c["wind_speed_10m"].isNull() ? NO_VALUE : (uint8_t)lroundf(c["wind_speed_10m"].as<float>());
  out.now.windDir = c["wind_direction_10m"].isNull() ? "" : windDir(c["wind_direction_10m"].as<float>());
  out.now.day = (c["is_day"] | 1) != 0;

  JsonObject h = doc["hourly"];
  JsonArray ht = h["time"];
  out.hours.clear();
  for (size_t i = 0; i < ht.size(); i++) {
    if (ht[i].isNull() || h["temperature_2m"][i].isNull() || h["weather_code"][i].isNull()) break;
    Hour x;
    x.at = ht[i].as<long>();
    x.temp = (int16_t)lroundf(h["temperature_2m"][i].as<float>());
    x.code = h["weather_code"][i].as<int>();
    x.rain = h["precipitation_probability"][i].isNull() ? NO_VALUE : (uint8_t)(h["precipitation_probability"][i].as<int>());
    x.day = (h["is_day"][i] | 1) != 0;
    out.hours.push_back(x);
  }
  JsonObject d = doc["daily"];
  JsonArray dt = d["time"];
  out.days.clear();
  for (size_t i = 0; i < dt.size(); i++) {
    if (dt[i].isNull() || d["temperature_2m_max"][i].isNull() || d["temperature_2m_min"][i].isNull() ||
        d["weather_code"][i].isNull())
      break;
    Day x;
    x.date = dt[i].as<long>();
    x.hi = (int16_t)lroundf(d["temperature_2m_max"][i].as<float>());
    x.lo = (int16_t)lroundf(d["temperature_2m_min"][i].as<float>());
    x.code = d["weather_code"][i].as<int>();
    x.rain = d["precipitation_probability_max"][i].isNull() ? NO_VALUE : (uint8_t)(d["precipitation_probability_max"][i].as<int>());
    out.days.push_back(x);
  }
  if (out.hours.empty() || out.days.empty()) {
    Serial.println("[weather] forecast has no hours or no days");
    return false;
  }
  return true;
}

// ---------- background task ----------

void task(void *) {
  String geoLoc;  // the location the coordinates below belong to
  float lat = 0, lon = 0;
  String place, cc;
  int lastUnits = -1;
  uint32_t lastOk = 0, lastTry = 0;
  int fails = 0;
  bool haveData = false, notFound = false;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!enabled) {
      st = State::Off;
      continue;
    }
    String loc;
    {
      Lock l;
      loc = wantLoc;
    }
    // A search from Settings > Weather runs here too, between forecasts.
    if (searchSt == Search::Busy) {
      String q;
      {
        Lock l;
        q = searchQ;
      }
      std::vector<Place> res;
      bool ok = false;
      String sName, sQual;
      bool guessed;
      splitQuery(q, sName, sQual, guessed);
      if (!sName.length()) sName = q;
      for (int attempt = 0; attempt < 2 && WiFi.status() == WL_CONNECTED; attempt++) {
        String body;
        if (httpGet("http://geocoding-api.open-meteo.com/v1/search?count=10&language=en&format=json&name=" + urlEncode(sName), body)) {
          JsonDocument doc;
          if (!deserializeJson(doc, body)) {
            ok = true;
            for (JsonObject r : doc["results"].as<JsonArray>()) {
              Place p;
              p.name = ascii((const char *)(r["name"] | ""));
              p.region = ascii((const char *)(r["admin1"] | ""));
              p.country = ascii((const char *)(r["country"] | ""));
              p.cc = (const char *)(r["country_code"] | "");
              p.lat = r["latitude"] | 0.0f;
              p.lon = r["longitude"] | 0.0f;
              res.push_back(p);
            }
          }
        }
        if (!ok) break;
        if (sQual.length()) {
          // Places matching "AZ" / "NZ" / "Ontario" first, the rest after.
          std::stable_partition(res.begin(), res.end(), [&](const Place &p) {
            return qualMatches(sQual, p.region, p.country, p.cc);
          });
          bool any = !res.empty() && qualMatches(sQual, res[0].region, res[0].country, res[0].cc);
          if (guessed && !any) {  // the last word wasn't a qualifier: search the whole text
            sName = q;
            sName.trim();
            sQual = "";
            res.clear();
            ok = false;
            continue;
          }
        }
        break;
      }
      {
        Lock l;
        found = res;
      }
      searchSt = ok ? Search::Done : Search::Failed;
      Serial.printf("[weather] search \"%s\": %s, %u places\n", q.c_str(), ok ? "ok" : "failed", (unsigned)res.size());
      for (size_t i = 0; i < res.size(); i++)
        Serial.printf("[weather]   %u: %s | %s | %s | %s\n", (unsigned)i, res[i].name.c_str(), res[i].region.c_str(),
                      res[i].country.c_str(), res[i].cc.c_str());
    }
    // A place picked in Settings > Weather: fetch it first; keep the current one if that fails.
    if (chooseSt == Choose::Busy) {
      String tl;
      {
        Lock l;
        tl = tryLoc;
      }
      bool ok = false;
      float nlat = 0, nlon = 0;
      String nplace, ncc;
      if (WiFi.status() == WL_CONNECTED && geocode(tl, nlat, nlon, nplace, ncc) == Geo::Found) {
        int u = (int)units();
        bool fc = !fahrenheitCountry(ncc);
        bool celsius = u == UNITS_C || (u == UNITS_AUTO && fc);
        Data fresh;
        String date;
        if (fetchForecast(nlat, nlon, celsius, fresh, date)) {
          maybeSetClock(date);
          if (!fresh.fetched) fresh.fetched = time(nullptr);
          fresh.place = nplace;
          fresh.lat = nlat;
          fresh.lon = nlon;
          fresh.cc = ncc;
          geoLoc = tl;
          lat = nlat;
          lon = nlon;
          place = nplace;
          cc = ncc;
          foundCelsius = fc;
          lastUnits = u;
          haveData = true;
          notFound = false;
          fails = 0;
          lastOk = lastTry = millis();
          {
            Lock l;
            data = std::move(fresh);
            wantLoc = tl;
            commitLoc = true;
          }
          ver++;
          st = State::Ready;
          ok = true;
        }
      }
      chooseSt = ok ? Choose::Done : Choose::Failed;
      Serial.printf("[weather] new location %s: %s\n", tl.c_str(), ok ? "set" : "couldn't fetch, keeping the old one");
      continue;
    }
    int u = (int)units();
    if (u != lastUnits && lastUnits >= 0 && haveData) force = true;
    bool locChanged = loc != geoLoc || (force && notFound);  // a refresh looks an unknown place up again
    bool due = force || locChanged || (!notFound && (!haveData || millis() - lastOk > REFRESH_MS));
    if (!due) continue;
    if (WiFi.status() != WL_CONNECTED) {
      if (!haveData) st = State::Waiting;
      continue;
    }
    if (fails && !force && millis() - lastTry < RETRY_MS[min(fails, 4) - 1]) continue;
    force = false;
    lastTry = millis();
    if (!haveData || locChanged) st = State::Loading;

    if (locChanged) {
      Geo g = geocode(loc, lat, lon, place, cc);
      if (g != Geo::Found) {
        if (g == Geo::NotFound) {
          // Not a network problem: stop until the location changes or a refresh is asked for.
          geoLoc = loc;
          haveData = false;
          notFound = true;
          fails = 0;
          {
            Lock l;
            data = Data();
          }
          ver++;
          st = State::NotFound;
          Serial.printf("[weather] location not found: %s\n", loc.c_str());
        } else {
          fails++;
          st = State::Failed;
        }
        continue;
      }
      geoLoc = loc;
      haveData = false;
      notFound = false;
    }

    Data fresh;
    uint32_t t0 = millis();
    foundCelsius = !fahrenheitCountry(cc);
    lastUnits = u;
    bool celsius = u == UNITS_C || (u == UNITS_AUTO && foundCelsius);
    String date;
    if (fetchForecast(lat, lon, celsius, fresh, date)) {
      maybeSetClock(date);
      if (!fresh.fetched) fresh.fetched = time(nullptr);
      fresh.place = place;
      fresh.lat = lat;
      fresh.lon = lon;
      fresh.cc = cc;
      {
        Lock l;
        data = std::move(fresh);
      }
      haveData = true;
      fails = 0;
      lastOk = millis();
      ver++;
      st = State::Ready;
      Serial.printf("[weather] updated in %lu ms: %d %s, code %d, %u hours, %u days\n",
                    (unsigned long)(millis() - t0), data.now.temp, celsius ? "C" : "F", data.now.code,
                    (unsigned)data.hours.size(), (unsigned)data.days.size());
    } else {
      fails++;
      st = haveData ? State::Ready : State::Failed;  // keep showing older data if there is some
      Serial.printf("[weather] fetch failed (%d in a row)\n", fails);
    }
  }
}

uint32_t lastTick = 0;
bool taskOk = true;

}  // namespace

Cond condOf(int c) {
  if (c <= 1) return CLEAR;
  if (c == 2) return PARTLY;
  if (c == 3) return CLOUDY;
  if (c == 45 || c == 48) return FOG;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return SNOW;
  if (c >= 95) return STORM;
  return RAIN;  // drizzle, rain, freezing rain, showers
}

const char *condName(int c) {
  switch (c) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Cloudy";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm with hail";
    default: return "Unknown";
  }
}

int Data::firstHour() const {
  time_t now = time(nullptr);
  for (int i = 0; i < (int)hours.size(); i++)
    if (hours[i].at + 3600 > now) return i;
  return max(0, (int)hours.size() - 1);
}

int Data::firstDay() const {
  time_t now = time(nullptr);
  for (int i = 0; i < (int)days.size(); i++)
    if (days[i].date + 86400 > now) return i;
  return max(0, (int)days.size() - 1);
}

void begin() {
  mtx = xSemaphoreCreateMutex();
  unitsNow = constrain(store::getInt("wxunits", UNITS_AUTO), 0, 2);
  tz::setCountry(store::getStr("wxcc"));
  enabled = net::configured();
  wantLoc = net::location();
  seenLoc = wantLoc;
  if (xTaskCreatePinnedToCore(task, "weather", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("[weather] no memory for the weather task: weather off");
    taskOk = false;
  }
}

void tick() {
  if (millis() - lastTick < 2000) return;
  lastTick = millis();
  enabled = taskOk && net::configured();
  httpTimeOk = net::autoTime();
  // A place tried in Settings > Weather worked: save it (here, on core 1).
  String commit;
  {
    Lock l;
    if (commitLoc) {
      commitLoc = false;
      commit = tryLoc;
    }
  }
  if (commit.length()) {
    net::setLocation(commit);
    seenLoc = commit;
  }
  String loc = net::location();
  String cc, zone, place;
  float plat = 0, plon = 0;
  int32_t off = 0;
  bool fresh = false;
  {
    Lock l;
    if (loc != seenLoc) {  // changed elsewhere (the setup page)
      seenLoc = loc;
      wantLoc = loc;
      force = true;  // skip any retry wait
    }
    if (ver != appliedVer && data.fetched) {
      appliedVer = ver;
      fresh = true;
      cc = data.cc;
      zone = data.zone;
      off = data.utcOffset;
      place = data.place;
      plat = data.lat;
      plon = data.lon;
    }
  }
  if (!fresh) return;
  // The country the clock and date formats follow.
  if (cc.length() && cc != store::getStr("wxcc")) store::setStr("wxcc", cc);
  if (cc.length()) tz::setCountry(cc);
  if (!zone.length()) return;
  if (store::getStr("tzloc") != loc) {
    // A location chosen since the zone was last set: its time zone becomes the device's. After
    // that the zone is the user's to change (Settings > Time zone) until the location changes.
    // A zone without a preset of its own follows the forecast's UTC offset instead.
    int c = tz::cityFor(zone.c_str(), off);
    if (c >= 0) {
      tz::clearLocationZone();
      if (c != tz::deviceCity()) tz::setDeviceCity(c);
    } else {
      tz::setLocationZone(off, place.length() ? place : zone, plat, plon, true);
    }
    Serial.printf("[weather] location's zone %s -> %s\n", zone.c_str(), c >= 0 ? tz::CITIES[c].name : "its UTC offset");
    store::setStr("tzloc", loc);
  } else if (tz::hasLocationZone() && off != tz::locationOffset()) {
    // The location's own zone moved (daylight saving): follow it if the device uses it.
    tz::setLocationZone(off, place.length() ? place : zone, plat, plon, tz::deviceCity() < 0);
  }
}

State state() { return st; }

bool get(Data &out) {
  Lock l;
  if (!data.fetched) return false;
  out = data;
  return true;
}

uint32_t version() { return ver; }

void refresh() {
  force = true;
  Serial.println("[weather] refresh requested");
}

void search(const String &query) {
  {
    Lock l;
    searchQ = query;
    found.clear();
  }
  searchSt = Search::Busy;
}

Search searchState() { return searchSt; }

std::vector<Place> searchResults() {
  Lock l;
  return found;
}

void choose(const Place &p) {
  char ll[48];
  snprintf(ll, sizeof(ll), "@%.4f,%.4f", p.lat, p.lon);
  String region = p.region.length() && p.region != p.name ? p.region + ", " + p.country : p.country;
  searchSt = Search::Idle;
  if (!enabled) {
    chooseSt = Choose::Failed;
    return;
  }
  {
    Lock l;
    tryLoc = String(ll) + "|" + p.name + "|" + region + "|" + p.cc;
  }
  chooseSt = Choose::Busy;
}

Choose chooseState() { return chooseSt; }

String locationLabel() {
  String loc = net::location();
  if (!loc.startsWith("@")) return loc;
  int p1 = loc.indexOf('|'), p2 = loc.indexOf('|', p1 + 1), p3 = loc.indexOf('|', p2 + 1);
  if (p1 < 0) return loc;
  String name = loc.substring(p1 + 1, p2 < 0 ? loc.length() : p2);
  String region = p2 < 0 ? String("") : loc.substring(p2 + 1, p3 < 0 ? loc.length() : p3);
  return region.length() ? name + ", " + region : name;
}

Units units() { return (Units)unitsNow; }
void setUnits(Units u) {
  store::setInt("wxunits", u);
  unitsNow = u;
  refresh();
}
bool autoCelsius() { return foundCelsius; }

String ascii(const String &in) {
  // U+00C0..U+017F -> base letter.
  static const char FOLD[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
  String out;
  const uint8_t *s = (const uint8_t *)in.c_str();
  while (*s) {
    uint8_t c = *s;
    if (c < 0x80) { out += (char)c; s++; continue; }
    uint32_t cp = 0;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    cp = c & (0x3F >> n);
    s++;
    for (int i = 0; i < n && (*s & 0xC0) == 0x80; i++, s++) cp = (cp << 6) | (*s & 0x3F);
    if (cp >= 0xC0 && cp < 0x180) out += FOLD[cp - 0xC0];
    else if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    // anything else (other scripts) is dropped
  }
  return out;
}

bool coords(float &lat, float &lon) {
  Lock l;
  if (!data.fetched) return false;
  lat = data.lat;
  lon = data.lon;
  return true;
}

}  // namespace weather
