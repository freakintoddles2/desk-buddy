// Wall clock. The board has no battery-backed clock, so after power-up the time comes from:
//   1. the firmware build time (testing stand-in), or
//   2. a "T<unix seconds>" line on USB serial (sent from the Mac after flashing).
// Later: a set-the-time screen when there's no Wi-Fi, and network time with Wi-Fi.
#pragma once

#include <Arduino.h>
#include <time.h>

namespace clockx {

void begin();
void poll();           // serial commands: T<unix seconds>, S screenshot, K<n>, P, H, W<+-1> (testing)
struct tm local();     // current local time
float hourOfDay();     // 0-24, with fraction
float utcOffsetHours();  // e.g. -7 for Arizona, 5.5 for India
void setLocal(int year, int month, int day, int hour, int minute);  // month 1-12

// True once the clock has been set since power-up: by network time, by hand, by serial T, or
// from a forecast's Date header. Until then it runs from the firmware's build time.
bool trusted();
void markTrusted();  // network time arrived (any task)
// Set the clock from a network source other than NTP (any task).
void setFromNetwork(time_t t, const char *source);

}  // namespace clockx
