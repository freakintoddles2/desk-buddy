#include "buzzer.h"

#include <math.h>

namespace buzzer {
namespace {

constexpr int PIN = 44;
constexpr uint8_t RES_BITS = 10;

Step steps[MAX_STEPS];
int stepCount = 0;
uint32_t dutyCounts = 512;
uint8_t repeats = 1;
uint16_t gap = 0;

TaskHandle_t task = nullptr;
volatile bool abortReq = false;
volatile bool playing = false;
TickType_t wake = 0;

void tone(uint32_t hz) {
  ledcChangeFrequency(PIN, hz, RES_BITS);
  ledcWrite(PIN, dutyCounts);
}
void silence() { ledcWrite(PIN, 0); }

// Sleep `ms` against a running deadline (so steps don't drift); false if aborted.
bool sleepMs(uint32_t ms) {
  while (ms--) {
    vTaskDelayUntil(&wake, 1);
    if (abortReq) return false;
  }
  return true;
}

bool playStep(const Step &s) {
  if (s.f0 == 0) {
    silence();
    return sleepMs(s.ms);
  }
  if (s.f0 == s.f1 || s.ms < 4) {
    tone(s.f0);
    return sleepMs(s.ms);
  }
  int n = s.ms / 2;
  float ratio = (float)s.f1 / s.f0;
  for (int i = 0; i < n; i++) {
    tone((uint32_t)lroundf(s.f0 * powf(ratio, (float)i / n)));
    if (!sleepMs(i == n - 1 ? s.ms - 2 * (n - 1) : 2)) return false;
  }
  return true;
}

void run(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    abortReq = false;
    wake = xTaskGetTickCount();
    bool ok = true;
    for (int r = 0; ok && r < repeats; r++) {
      for (int i = 0; ok && i < stepCount; i++) ok = playStep(steps[i]);
      if (ok && r < repeats - 1 && gap) {
        silence();
        ok = sleepMs(gap);
      }
    }
    silence();
    playing = false;
  }
}

}  // namespace

void begin() {
  ledcAttach(PIN, 2300, RES_BITS);
  silence();
  // Core 0 (the UI runs on core 1). It sleeps between 1 ms ticks, so it never trips the idle watchdog.
  xTaskCreatePinnedToCore(run, "buzzer", 3072, nullptr, 3, &task, 0);
}

void stop() {
  if (!playing) return;
  abortReq = true;
  for (int i = 0; playing && i < 20; i++) delay(1);
}

bool busy() { return playing; }

void play(const Step *src, int n, uint8_t dutyPct, uint8_t repeat, uint16_t gapMs) {
  if (!task) return;
  stop();
  n = constrain(n, 0, MAX_STEPS);
  if (n == 0) return;
  memcpy(steps, src, n * sizeof(Step));
  stepCount = n;
  dutyCounts = (uint32_t)constrain((int)dutyPct, 1, 50) * ((1 << RES_BITS) - 1) / 100;
  repeats = max<uint8_t>(repeat, 1);
  gap = gapMs;
  playing = true;
  xTaskNotifyGive(task);
}

void chirp(uint16_t hz, uint16_t ms) {
  Step s = {hz, hz, ms};
  play(&s, 1, 50);
}

void playCommand(const char *s) {
  while (*s == ' ') s++;
  if (!*s) {
    stop();
    Serial.println("[snd] stopped");
    return;
  }
  static Step buf[MAX_STEPS];
  int duty = 50, rep = 1, gp = 0;
  const char *p = strchr(s, ':');
  if (!p || sscanf(s, "%d,%d,%d", &duty, &rep, &gp) < 1) {
    Serial.println("[snd] bad command");
    return;
  }
  int n = 0;
  uint32_t total = 0;
  for (p++; *p && n < MAX_STEPS;) {
    int f0, f1, ms;
    if (sscanf(p, "%d,%d,%d", &f0, &f1, &ms) != 3) break;
    buf[n++] = {(uint16_t)constrain(f0, 0, 20000), (uint16_t)constrain(f1, 0, 20000), (uint16_t)constrain(ms, 1, 10000)};
    total += ms;
    const char *q = strchr(p, ';');
    if (!q) break;
    p = q + 1;
  }
  if (rep < 1) rep = 1;
  play(buf, n, duty, rep, gp);
  Serial.printf("[snd] playing %d steps, %lu ms, duty %d%%, x%d\n", n, (unsigned long)total, duty, rep);
}

}  // namespace buzzer
