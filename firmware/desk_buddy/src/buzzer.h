// Optional KY-006 passive buzzer on the UART1 plug's RX pin (GPIO44).
// Sounds play on a small core-0 task, so the UI loop never waits on them.
#pragma once

#include <Arduino.h>

namespace buzzer {

// One step of a sound: f0 == f1 is a tone, f0 != f1 an exponential slide (stepped every 2 ms),
// f0 == 0 a rest. Same format as the sound lab's export.
struct Step {
  uint16_t f0, f1, ms;
};

constexpr int MAX_STEPS = 400;

void begin();
// Play `n` steps (copied), replacing whatever is playing. dutyPct: 50 High, 15 Med, 5 Low.
void play(const Step *steps, int n, uint8_t dutyPct = 50, uint8_t repeat = 1, uint16_t gapMs = 0);
// A single short tone (knob test).
void chirp(uint16_t hz, uint16_t ms);
void stop();
bool busy();

// Serial test command, the line after 'Z': "<duty>,<repeat>,<gap>:f0,f1,ms;f0,f1,ms;..."
// An empty line stops. Prints "[snd] ..." with the result.
void playCommand(const char *s);

}  // namespace buzzer
