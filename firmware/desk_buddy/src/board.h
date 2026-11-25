// Board layer: everything that touches CrowPanel 2.1" hardware lives here.
// The UI only sees knob steps, knob press events, brightness, the LED and LVGL.
#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace board {

constexpr uint16_t SCREEN_W = 480;
constexpr uint16_t SCREEN_H = 480;

// Down: the knob went down (Press comes on release, so a hold can be told apart). Games that
// time a press (Reflex) use Down and downAt().
enum class Button { None, Down, Press, LongPress, HoldHome };
uint32_t downAt();  // millis() when the knob's contact first closed, for the last Down

// Power, panel, touch, LVGL display and touch input, knob interrupt, backlight, LED.
void begin();

// Knob detents since the last call. Positive = clockwise.
int32_t takeKnobSteps();

// Knob switch, debounced. Press fires on release; LongPress fires once while held.
Button pollButton();

// 0-100 %.
void setBrightness(uint8_t pct);
void setBacklight(float pct);           // 0-100 %, fractional (12-bit PWM)
void setBacklightDuty(uint32_t duty);   // raw 0-4095, for testing the low end
void setLed(bool on);

// The LVGL touch input device (for tuning long-press time and gestures).
lv_indev_t *touchIndev();

// The panel's framebuffer (480x480 RGB565, PSRAM), for animations that bypass LVGL (the
// wheel's spinning band). Call framebufferDone() after writing so the panel sees the pixels.
uint16_t *framebuffer();
void framebufferDone();
void framebufferDone(int y0, int y1);  // only rows [y0, y1)

// Every touch sample LVGL reads goes through `hook` first; returning true hides it from LVGL
// (the fast travel tray owns that touch).
void setTouchHook(bool (*hook)(bool down, int x, int y));
// The touch controller right now, bypassing LVGL (for the tray, which polls every frame).
bool readTouch(int16_t &x, int16_t &y);
// Where the current (or last) touch started, for edge swipes (Home's swipe up from the bottom).
void touchStart(int16_t &x, int16_t &y);
// Wait until every flush LVGL handed over has been copied.
void waitFlushIdle();

// Dump the panel's framebuffer over USB serial (for tools/screenshot.py):
// "SNAP <w> <h>\n", then w*h RGB565 little-endian pixels, then "\nEND\n".
void sendScreenshot();

// Simulated input from USB serial (testing): knob steps and button events.
void injectKnob(int32_t steps);
void injectButton(Button b);

constexpr uint32_t LONG_PRESS_MS = 700;
constexpr uint32_t HOME_HOLD_MS = 3000;  // hold this long (knob or a still finger): go Home

}  // namespace board
