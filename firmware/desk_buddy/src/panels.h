#pragma once

#include "app.h"

Panel *makeHomePanel();      // home.cpp
void homeTestSkyCode(int code);  // home.cpp: serial X<code>, Living sky shows that weather (-1: the real one)
Panel *makeSettingsPanel();  // settings.cpp
Screen *makeBrightnessScreen();  // settings.cpp; also the tray's quick button
Panel *makeFocusPanel();     // focus.cpp
Panel *makeTimersPanel();    // timers_panel.cpp
Panel *makeBreathePanel();   // breathe.cpp
Panel *makeWeatherPanel();   // weather_panel.cpp
Panel *makeWorldPanel();     // world.cpp
Panel *makePlayPanel();      // play.cpp
Panel *makeHabitsPanel();    // records_panels.cpp
Panel *makeDaysPanel();      // records_panels.cpp

// All panels in the default swipe order.
std::vector<Panel *> makePanels();
