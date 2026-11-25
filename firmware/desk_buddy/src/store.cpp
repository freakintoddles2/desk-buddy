#include "store.h"

#include <Preferences.h>
#include <vector>

namespace store {
namespace {

Preferences prefs;
uint8_t brightnessPct = 80;
uint32_t boots = 0;
bool brightDirty = false;
uint32_t changedAt = 0;
constexpr uint32_t SAVE_DELAY_MS = 1500;  // avoid a flash write on every knob detent
struct Pending {  // setIntLater values not yet in flash
  String key;
  int32_t value;
};
std::vector<Pending> pending;
uint32_t pendingAt = 0;

std::vector<String> order;
std::vector<String> hiddenIds;
uint8_t reminders[REMINDER_COUNT] = {0, 0, 0};
const char *REMINDER_KEYS[REMINDER_COUNT] = {"r_stand", "r_water", "r_eyes"};

std::vector<String> split(const String &csv) {
  std::vector<String> out;
  int start = 0;
  while (start < (int)csv.length()) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    if (comma > start) out.push_back(csv.substring(start, comma));
    start = comma + 1;
  }
  return out;
}

String join(const std::vector<String> &v) {
  String s;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) s += ',';
    s += v[i];
  }
  return s;
}

}  // namespace

void begin() {
  prefs.begin("deskbuddy", false);
  brightnessPct = constrain(prefs.getUChar("bright", 80), 1, 100);
  boots = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", boots);
  order = split(prefs.getString("order", ""));
  hiddenIds = split(prefs.getString("hidden", ""));
  for (int i = 0; i < REMINDER_COUNT; i++) reminders[i] = prefs.getUChar(REMINDER_KEYS[i], 0);
}

void tick() {
  if (brightDirty && millis() - changedAt >= SAVE_DELAY_MS) {
    prefs.putUChar("bright", brightnessPct);
    brightDirty = false;
    Serial.printf("[store] saved brightness %u%%\n", brightnessPct);
  }
  if (!pending.empty() && millis() - pendingAt >= SAVE_DELAY_MS) {
    for (const Pending &p : pending) prefs.putInt(p.key.c_str(), p.value);
    pending.clear();
  }
}

uint8_t brightness() { return brightnessPct; }

void setBrightness(uint8_t pct) {
  brightnessPct = constrain(pct, 1, 100);
  brightDirty = true;
  changedAt = millis();
}

uint32_t bootCount() { return boots; }

std::vector<String> panelOrder() { return order; }

void setPanelOrder(const std::vector<String> &ids) {
  order = ids;
  prefs.putString("order", join(order));
}

bool hidden(const char *id) {
  for (auto &h : hiddenIds)
    if (h == id) return true;
  return false;
}

void setHidden(const char *id, bool hide) {
  std::vector<String> next;
  for (auto &h : hiddenIds)
    if (h != id) next.push_back(h);
  if (hide) next.push_back(id);
  hiddenIds = next;
  prefs.putString("hidden", join(hiddenIds));
}

int32_t getInt(const char *key, int32_t def) {
  for (const Pending &p : pending)
    if (p.key == key) return p.value;
  return prefs.getInt(key, def);
}

void setIntLater(const char *key, int32_t value) {
  pendingAt = millis();
  for (Pending &p : pending)
    if (p.key == key) { p.value = value; return; }
  pending.push_back({key, value});
}
// isKey first: Preferences logs an error for every missing key otherwise.
String getStr(const char *key, const char *def) { return prefs.isKey(key) ? prefs.getString(key, def) : String(def); }
void setStr(const char *key, const String &value) { prefs.putString(key, value); }
void remove(const char *key) { prefs.remove(key); }
bool wifi() { return getStr("ssid").length() > 0; }
void setInt(const char *key, int32_t value) {
  for (size_t i = 0; i < pending.size(); i++)
    if (pending[i].key == key) { pending.erase(pending.begin() + i); break; }
  prefs.putInt(key, value);
}

uint8_t reminderMinutes(Reminder r) { return reminders[r]; }

void setReminderMinutes(Reminder r, uint8_t minutes) {
  reminders[r] = minutes;
  prefs.putUChar(REMINDER_KEYS[r], minutes);
}

}  // namespace store
