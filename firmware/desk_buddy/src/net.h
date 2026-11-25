// Optional Wi-Fi: setup mode (the device's own network + a captive setup page on the phone),
// saved credentials, automatic reconnect, and network time. The device works fully without it.
#pragma once

#include <Arduino.h>

namespace net {

enum class Status { Off, Connecting, Connected, Failed };

void begin();  // connect with saved credentials, if any
void tick();   // call from loop()

bool configured();  // credentials saved
String ssid();
Status status();
bool timeSynced();  // network time has set the clock since boot
bool autoTime();    // network time on (Settings > Date & time)
void setAutoTime(bool on);
void forget();
// Test (serial N): drop the connection with the system's reconnect off, as after a router reboot.
void testDrop();

// Setup mode: the device runs its own open network; joining it opens the setup page.
void startSetup();
void stopSetup();
bool inSetup();
String apName();
// The setup page was submitted: true = joining Wi-Fi (watch status()), false = offline.
bool takeSaved(bool &wifi);

String location();  // weather location as typed ("Phoenix, AZ" or a ZIP), or "@lat,lon|..." when picked on the device
void setLocation(const String &loc);

}  // namespace net
