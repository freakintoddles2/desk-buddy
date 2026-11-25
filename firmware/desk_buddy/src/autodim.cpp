#include "autodim.h"

#include <math.h>

#include "board.h"
#include "clock.h"
#include "store.h"
#include "sun.h"
#include "tz.h"
#include "weather.h"

namespace autodim {
namespace {

constexpr float EVENING_PCT = 30;
constexpr int NIGHT_MIN_MAX = 25;     // Night min tops out here (2026-11-11)
constexpr float DUSK_HALF_H = 0.5f;     // dusk ramp: sunset -30 min to +30 min
constexpr float MORNING_H = 1, BED_RAMP_H = 1;
constexpr uint32_t BUMP_MS = 20000;     // input at night: 30% for this long after the last input
constexpr float TAU_UP_S = 0.1f;        // reaches the bump level in ~0.3 s
constexpr float TAU_S = 0.7f;           // everything else eases over a couple of seconds

bool on = true;
int wakeMin = 360, bedMin = 1320, maxPct = 80, minPct = 5;
int previewPct = 0;
uint32_t previewAt = 0;

float cur = 80;          // shown level, %
float sched = 80;        // schedule's level, recomputed each second
Phase ph = PAUSED;
uint32_t lastCalc = 0, lastTick = 0;
int32_t lastDuty = -1;

bool overrideOn = false;
float overridePct = 0;
Phase overridePhase = PAUSED;
time_t overrideEndsAt = 0;

uint32_t inputAt = 0;
bool anyInput = false;

float ffSpeed = 0;       // fast-forward for testing; 0 = real time
time_t ffStart = 0;
uint32_t ffMillis = 0;
time_t lastPrinted = 0;

time_t nowT() {
  if (ffSpeed > 1) return ffStart + (time_t)((double)(millis() - ffMillis) * ffSpeed / 1000.0);
  return time(nullptr);
}

float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// From va at x=a to vb at x=b, in log space with eased ends.
float ramp(float x, float a, float b, float va, float vb) {
  float u = clampf((x - a) / (b - a), 0, 1);
  u = u * u * (3 - 2 * u);
  return expf(logf(va) + (logf(vb) - logf(va)) * u);
}

// Hours since wake time, 0-24.
float rel(float h, float wakeH) { return fmodf(h - wakeH + 48, 24); }

void writeBacklight() {
  int32_t d = lroundf(clampf(cur, 0, 100) * 40.95f);
  if (d != lastDuty) {
    lastDuty = d;
    board::setBacklightDuty(d);
  }
}

}  // namespace

const char *phaseName(Phase p) {
  static const char *const N[] = {"morning", "day", "dusk", "evening", "bedtime", "night", "paused"};
  return N[p];
}

float targetAt(time_t t, Phase *out) {
  const float D = maxPct, E = fminf(EVENING_PCT, D), N = fminf((float)minPct, D);
  if (!clockx::trusted()) {  // no real time yet: never dim to 5% at noon because the clock is wrong
    if (out) *out = PAUSED;
    return D;
  }
  struct tm l;
  localtime_r(&t, &l);
  float h = l.tm_hour + l.tm_min / 60.0f + l.tm_sec / 3600.0f;
  float lat, lon;
  if (!weather::coords(lat, lon)) tz::fallbackCoords(lat, lon);
  SunTimes st = sunTimes(l, tz::offset(tz::devicePosix(), t) / 3600.0f, lat, lon);

  const float wakeH = wakeMin / 60.0f;
  const float r = rel(h, wakeH);
  const float bedR = clampf(rel(bedMin / 60.0f, wakeH), 3, 23.5f);

  // Sun: day max until dusk, then the evening level. Midnight sun: no dusk. Polar night: the
  // sun's "set" is solar noon. A sunset within the morning ramp counts as just after it.
  float sunR = st.polar == 1 ? 99 : fmaxf(rel(st.set, wakeH), MORNING_H + DUSK_HALF_H);
  auto sunLevel = [&](float x) { return st.polar == 1 ? D : ramp(x, sunR - DUSK_HALF_H, sunR + DUSK_HALF_H, D, E); };

  float S = sunLevel(r);
  float M = ramp(r, 0, MORNING_H, N, D);
  float B = r < bedR - BED_RAMP_H ? 100 : ramp(r, bedR - BED_RAMP_H, bedR, sunLevel(bedR - BED_RAMP_H), N);
  float level = fminf(M, fminf(S, B));

  if (out) {
    if (r < MORNING_H) *out = MORNING;
    else if (r >= bedR) *out = NIGHT;
    else if (r >= bedR - BED_RAMP_H) *out = BEDTIME;
    else if (r < sunR - DUSK_HALF_H) *out = DAY;
    else if (r < sunR + DUSK_HALF_H) *out = DUSK;
    else *out = EVENING;
  }
  return level;
}

void begin() {
  on = store::getInt("ad_on", 1) != 0;
  wakeMin = constrain(store::getInt("ad_wake", 360), 4 * 60, 11 * 60);
  bedMin = store::getInt("ad_bed", 1320);
  maxPct = constrain(store::getInt("ad_max", 80), 1, 100);
  minPct = constrain(store::getInt("ad_min", 5), 1, NIGHT_MIN_MAX);
  sched = targetAt(nowT(), &ph);
  cur = on ? sched : store::brightness();
  lastCalc = lastTick = millis();
  writeBacklight();
}

void tick() {
  uint32_t now = millis();
  if (now - lastCalc >= (ffSpeed > 1 ? 50u : 1000u)) {
    lastCalc = now;
    time_t t = nowT();
    Phase p;
    sched = targetAt(t, &p);
    ph = p;
    if (overrideOn && ph != overridePhase) {
      overrideOn = false;
      Serial.printf("[autodim] %s: manual level ends, auto again\n", phaseName(ph));
    }
    if (ffSpeed > 1 && t - lastPrinted >= 15 * 60) {
      lastPrinted = t - t % (15 * 60);
      printState();
    }
  }
  float tgt;
  bool bump = false;
  bool previewing = previewPct > 0 && now - previewAt < 1000;
  if (previewing) tgt = previewPct;
  else if (!on) tgt = store::brightness();
  else {
    tgt = overrideOn ? overridePct : sched;
    float E = fminf(EVENING_PCT, maxPct);
    // The bump only lifts the schedule; a manual level (any value, 1-100%) is never raised.
    if (!overrideOn && anyInput && now - inputAt < BUMP_MS && tgt < E) { tgt = E; bump = true; }
  }
  float dt = (now - lastTick) / 1000.0f;
  lastTick = now;
  if (fabsf(tgt - cur) < 0.01f) cur = tgt;
  else {
    float tau = bump && tgt > cur ? TAU_UP_S : TAU_S;
    float k = 1 - expf(-dt / tau);
    cur = expf(logf(fmaxf(cur, 0.1f)) + (logf(fmaxf(tgt, 0.1f)) - logf(fmaxf(cur, 0.1f))) * k);
  }
  writeBacklight();
}

bool enabled() { return on; }
void setEnabled(bool v) {
  on = v;
  overrideOn = false;
  store::setInt("ad_on", v ? 1 : 0);
}
int wakeMinutes() { return wakeMin; }
void setWakeMinutes(int m) { wakeMin = constrain(m, 4 * 60, 11 * 60); store::setInt("ad_wake", wakeMin); }
int bedMinutes() { return bedMin; }
void setBedMinutes(int m) { bedMin = ((m % 1440) + 1440) % 1440; store::setInt("ad_bed", bedMin); }
int dayMax() { return maxPct; }
void setDayMax(int pct) { maxPct = constrain(pct, 1, 100); store::setIntLater("ad_max", maxPct); }
int nightMin() { return minPct; }
void setNightMin(int pct) { minPct = constrain(pct, 1, NIGHT_MIN_MAX); store::setIntLater("ad_min", minPct); }
void preview(int pct) {
  if (pct != previewPct) { cur = pct; writeBacklight(); }  // the dial answers at once
  previewPct = constrain(pct, 1, 100);
  previewAt = millis();
}

float current() { return cur; }
Phase phase() { return ph; }
bool overriding() { return overrideOn; }
time_t overrideEnds() { return overrideOn ? overrideEndsAt : 0; }

void setManual(int pct) {
  pct = constrain(pct, 1, 100);
  if (!on) {
    store::setBrightness(pct);
  } else {
    overrideOn = true;
    overridePct = pct;
    overridePhase = ph;
    // The next phase change, found by stepping a minute at a time (at most a day, ~1440 cheap calls).
    overrideEndsAt = 0;
    if (ph != PAUSED) {
      time_t t = nowT();
      t -= t % 60;
      for (int i = 1; i <= 1440; i++) {
        Phase p;
        targetAt(t + i * 60, &p);
        if (p != ph) { overrideEndsAt = t + i * 60; break; }
      }
    }
  }
  cur = pct;  // the dial answers at once, no easing
  writeBacklight();
}

void input() {
  inputAt = millis();
  anyInput = true;
}

void fastForward(float speed, time_t start) {
  if (speed <= 1) {
    ffSpeed = 0;
    Serial.println("[autodim] fast-forward off");
  } else {
    ffStart = start ? start : time(nullptr);
    ffMillis = millis();
    ffSpeed = speed;
    lastPrinted = 0;
    Serial.printf("[autodim] fast-forward x%.0f\n", speed);
  }
  lastCalc = 0;
}

void printState() {
  time_t t = nowT();
  struct tm l;
  localtime_r(&t, &l);
  float lat, lon;
  if (!weather::coords(lat, lon)) tz::fallbackCoords(lat, lon);
  SunTimes st = sunTimes(l, tz::offset(tz::devicePosix(), t) / 3600.0f, lat, lon);
  auto hm = [](float h) { h = fmodf(h + 48, 24); int m = lroundf(h * 60); static char b[2][8]; static int i; i ^= 1;
                          snprintf(b[i], 8, "%02d:%02d", (m / 60) % 24, m % 60); return b[i]; };
  Serial.printf("[autodim] %02d:%02d %s, target %.1f%%, shown %.1f%% (%s%s) | max %d, min %d, wake %02d:%02d, bed %02d:%02d, "
                "sun %s-%s%s\n",
                l.tm_hour, l.tm_min, phaseName(ph), sched, cur, on ? "auto" : "off", overrideOn ? ", manual" : "",
                maxPct, minPct, wakeMin / 60, wakeMin % 60, bedMin / 60, bedMin % 60, hm(st.rise), hm(st.set),
                st.polar ? (st.polar > 0 ? " (midnight sun)" : " (polar night)") : "");
}

}  // namespace autodim
