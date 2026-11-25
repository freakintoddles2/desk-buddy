// Settings and data kept in flash (NVS). Stands in for the faux display's localStorage.
#pragma once

#include <Arduino.h>
#include <vector>

namespace store {

void begin();
// Call from loop(); writes pending changes once input has been idle for a moment.
void tick();

uint8_t brightness();               // 1-100 %
void setBrightness(uint8_t pct);    // written after a short idle delay (knob turns)

uint32_t bootCount();               // incremented once per boot

// Panel order (ids) as the user arranged it; ids not listed keep their default position.
std::vector<String> panelOrder();
void setPanelOrder(const std::vector<String> &ids);
bool hidden(const char *id);
void setHidden(const char *id, bool hide);

// Break reminders: minutes between reminders, 0 = off.
enum Reminder { STAND, WATER, EYES, REMINDER_COUNT };
uint8_t reminderMinutes(Reminder r);
void setReminderMinutes(Reminder r, uint8_t minutes);

// Small named settings (focus length, breathing pattern, focus log). Keys are <= 15 chars.
int32_t getInt(const char *key, int32_t def);
void setInt(const char *key, int32_t value);
// For values the knob steps through: kept in RAM and written to flash once the knob has been
// still for a moment (one write per adjustment, not one per click). getInt sees it at once.
void setIntLater(const char *key, int32_t value);

String getStr(const char *key, const char *def = "");
void setStr(const char *key, const String &value);
void remove(const char *key);

// True once Wi-Fi has been set up (credentials saved), connected or not.
bool wifi();

}  // namespace store
