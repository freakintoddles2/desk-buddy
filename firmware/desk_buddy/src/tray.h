// Fast travel tray (option D in fasttravel-mockups.html; ftShow/ftLayout in display.html).
// Pull down from the top edge: a sheet with every shown panel slides down over the screen.
// Keep the finger down and slide or flick sideways (same glide as the lists), let go and it
// jumps there. Let go without sliding and the tray stays open: flick, rotate, tap or press to
// pick; swipe up or hold to close. Left resting on a panel for 5 s, it goes there. It sits
// on top of the normal nav; it doesn't replace it.
//
// The tray draws straight into the panel's framebuffer: through LVGL a sheet sliding over the
// whole screen would mean full-screen redraws (~8 fps). LVGL's invalidation is paused while it
// is open, and the screen is redrawn by LVGL once it closes.
#pragma once

#include <stdint.h>

namespace tray {

// Paints the sheet's background on core 0 (once, at boot).
void begin();

// True from the moment a pull is recognized until the tray has slid away again.
bool live();

// Every touch sample LVGL reads (board.cpp). Returns true when the tray owns the touch, in
// which case LVGL is told the screen isn't touched.
bool touchSample(bool down, int x, int y);

// Called every loop by the app. Polls touch while live, animates and draws.
void loop();

// Knob and button while live (the app routes them here).
void rotate(int dir);
void press();
void hold();      // knob hold: close
void home();      // 3 s hold: go Home
void swipe(int dir);

// Serial tests: D opens/closes; Y<n> plays a scripted pull then a flick n panels over.
void testToggle();
void testFlick(int items);
// Serial Q: tap the brightness quick button (tray open).
void testQuick();

}  // namespace tray
