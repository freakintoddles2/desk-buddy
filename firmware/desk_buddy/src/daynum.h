// Calendar days as plain numbers (days since 1970-01-01), so "days until" and streaks are
// simple subtraction. Algorithms from Howard Hinnant's chrono date paper (public domain).
#pragma once

#include <Arduino.h>

#include "clock.h"
#include "tz.h"

namespace daynum {

struct Ymd {
  int y, m, d;  // m 1-12
};

inline int32_t fromYmd(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

inline Ymd toYmd(int32_t z) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = (int)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const int d = doy - (153 * mp + 2) / 5 + 1;
  const int m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2), m, d};
}

// 0 = Sunday.
inline int weekday(int32_t z) { return (int)((z % 7 + 11) % 7); }

inline int daysInMonth(int y, int m) {
  static const uint8_t DIM[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : DIM[m - 1];
}

// Today on the device's clock.
inline int32_t today() {
  struct tm t = clockx::local();
  return fromYmd(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

inline const char *dayShort(int wd) {
  static const char *N[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return N[wd];
}
inline const char *dayLong(int wd) {
  static const char *N[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  return N[wd];
}
inline const char *monthShort(int m) {
  static const char *N[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return N[m - 1];
}
inline const char *monthLong(int m) {
  static const char *N[] = {"January", "February", "March", "April", "May", "June",
                            "July", "August", "September", "October", "November", "December"};
  return N[m - 1];
}

// "Sat, Dec 20" or "Sat 20 Dec" (tz::dayFirst); the year is added when it isn't this year.
inline String shortText(int32_t z) {
  Ymd a = toYmd(z), now = toYmd(today());
  String s = tz::dateText(dayShort(weekday(z)), monthShort(a.m), a.d);
  if (a.y != now.y) s += (tz::dayFirst() ? String(" ") : String(", ")) + a.y;
  return s;
}

// "Saturday, December 20, 2026" or "Saturday 20 December 2026".
inline String longText(int32_t z) {
  Ymd a = toYmd(z);
  return tz::dateText(dayLong(weekday(z)), monthLong(a.m), a.d) + (tz::dayFirst() ? " " : ", ") + a.y;
}

}  // namespace daynum
