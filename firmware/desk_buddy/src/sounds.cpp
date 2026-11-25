#include "sounds.h"

#include <Arduino.h>

#include <esp_heap_caps.h>
#include <vector>

#include "store.h"

namespace sounds {
namespace {

Volume vol = VOL_MED;
bool clicksOn = true;
bool playingClick = false;   // what's on the buzzer now is a click (another sound may cut in)
int16_t picks[EVENT_COUNT];  // sound per event, -1 = none
uint32_t lastTick = 0;

// Each event's own sound from the sound lab (NVS "c_<event id>", same text as the C command),
// parsed into PSRAM.
struct Custom {
  String name;
  buzzer::Step *steps = nullptr;
  uint16_t n = 0;
  uint8_t repeat = 1;
  uint16_t gap = 0;
  bool soft = false;
};
Custom customs[EVENT_COUNT];
constexpr int CUSTOM_MAX_TEXT = 3900;  // NVS strings top out at 4000 bytes

String customKey(int e) { return String("c_") + EVENTS[e].id; }

// "<name>|<repeat>,<gap>,<soft>:f0,f1,ms;f0,f1,ms;..." -> c. False if it doesn't parse.
bool parseCustom(const String &text, Custom &c) {
  int bar = text.indexOf('|'), colon = text.indexOf(':', bar + 1);
  if (bar < 1 || colon < 0) return false;
  int rep = 1, gap = 0, soft = 0;
  if (sscanf(text.substring(bar + 1, colon).c_str(), "%d,%d,%d", &rep, &gap, &soft) < 1) return false;
  std::vector<buzzer::Step> st;
  const char *p = text.c_str() + colon + 1;
  while (*p && (int)st.size() < buzzer::MAX_STEPS) {
    unsigned f0, f1, ms;
    if (sscanf(p, "%u,%u,%u", &f0, &f1, &ms) != 3) return false;
    st.push_back({(uint16_t)constrain(f0, 0u, 10000u), (uint16_t)constrain(f1, 0u, 10000u), (uint16_t)constrain(ms, 1u, 3000u)});
    const char *semi = strchr(p, ';');
    if (!semi) break;
    p = semi + 1;
  }
  if (st.empty()) return false;
  auto *buf = (buzzer::Step *)heap_caps_malloc(st.size() * sizeof(buzzer::Step), MALLOC_CAP_SPIRAM);
  if (!buf) return false;
  memcpy(buf, st.data(), st.size() * sizeof(buzzer::Step));
  heap_caps_free(c.steps);
  c.steps = buf;
  c.n = st.size();
  c.name = text.substring(0, bar);
  c.repeat = constrain(rep, 1, 20);
  c.gap = constrain(gap, 0, 5000);
  c.soft = soft != 0;
  return true;
}
void dropCustom(int e) {
  heap_caps_free(customs[e].steps);
  customs[e] = Custom();
}

// LEDC duty % per volume; a soft sound uses the step below (Low's step below is 2%).
constexpr uint8_t DUTY[] = {0, 5, 15, 50};
constexpr uint8_t SOFT_DUTY[] = {0, 2, 5, 15};
constexpr uint32_t TICK_MIN_GAP_MS = 30;

bool isClick(Event e) { return e == KNOB_TICK || e == KNOB_PRESS || e == KEY_TYPE || e == KEY_DELETE; }

int findSound(const String &id) {
  if (id == "-") return -1;
  if (id == "*") return CUSTOM;
  for (int i = 0; i < SOUND_COUNT; i++)
    if (id == SOUNDS[i].id) return i;
  return -2;  // unknown (removed from the lab): use the default
}

// Saved as "event=sound,event=sound" with the lab's ids, only for events not on their default.
void savePicks() {
  String out;
  for (int e = 0; e < EVENT_COUNT; e++) {
    if (picks[e] == EVENTS[e].def) continue;
    if (out.length()) out += ',';
    out += EVENTS[e].id;
    out += '=';
    out += picks[e] == CUSTOM ? "*" : picks[e] < 0 ? "-" : SOUNDS[picks[e]].id;
  }
  store::setStr("sndpick", out);
}

void start(const Sound *s, bool click) {
  if (vol == VOL_OFF || !s || !s->n) return;
  if (click && buzzer::busy() && !playingClick) return;  // don't cut a real sound short
  buzzer::play(s->steps, s->n, s->soft ? SOFT_DUTY[vol] : DUTY[vol], s->repeat, s->gap);
  playingClick = click;
}

}  // namespace

void begin() {
  vol = (Volume)constrain(store::getInt("sndvol", VOL_MED), VOL_OFF, VOL_HIGH);
  clicksOn = store::getInt("sndclick", 1) != 0;
  for (int e = 0; e < EVENT_COUNT; e++) {
    picks[e] = EVENTS[e].def;
    String text = store::getStr(customKey(e).c_str());
    if (text.length()) parseCustom(text, customs[e]);
  }
  String saved = store::getStr("sndpick");
  int at = 0;
  while (at < (int)saved.length()) {
    int comma = saved.indexOf(',', at);
    if (comma < 0) comma = saved.length();
    String pair = saved.substring(at, comma);
    at = comma + 1;
    int eq = pair.indexOf('=');
    if (eq < 0) continue;
    String ev = pair.substring(0, eq);
    int s = findSound(pair.substring(eq + 1));
    for (int e = 0; e < EVENT_COUNT; e++)
      if (ev == EVENTS[e].id && (s >= -1 || (s == CUSTOM && customs[e].n))) picks[e] = s;
  }
}

void play(Event e) {
  if (e >= EVENT_COUNT) return;
  bool click = isClick(e);
  if (click && !clicksOn) return;
  if (e == KNOB_TICK) {
    uint32_t now = millis();
    if (now - lastTick < TICK_MIN_GAP_MS) return;
    lastTick = now;
  }
  if (picks[e] >= 0) start(&SOUNDS[picks[e]], click);
  else if (picks[e] == CUSTOM) playSound(CUSTOM, e);
}

void reflexResult(int ms) {
  int i = picks[REFLEX_RESULT];
  if (i == CUSTOM) { play(REFLEX_RESULT); return; }
  if (i < 0) return;
  if (!SOUNDS[i].dynamic) { play(REFLEX_RESULT); return; }
  // 150 ms or faster: G7 (3136 Hz); 450 ms or slower: G6 (1568 Hz); in between on a log scale.
  float t = constrain((ms - 150) / 300.0f, 0.0f, 1.0f);
  uint16_t f = (uint16_t)lroundf(3136 * powf(0.5f, t));
  buzzer::Step st{f, f, 60};
  Sound s = SOUNDS[i];
  s.steps = &st;
  s.n = 1;
  start(&s, false);
}

void preview() { play(HABIT_DONE); }

Volume volume() { return vol; }
void setVolume(Volume v) {
  vol = (Volume)constrain((int)v, (int)VOL_OFF, (int)VOL_HIGH);
  store::setInt("sndvol", vol);
}
const char *volumeName(Volume v) {
  static const char *const N[] = {"Off", "Low", "Medium", "High"};
  return N[constrain((int)v, 0, 3)];
}
bool clicks() { return clicksOn; }
void setClicks(bool on) {
  clicksOn = on;
  store::setInt("sndclick", on);
}

int soundFor(Event e) { return e < EVENT_COUNT ? picks[e] : -1; }
const char *pickName(Event e) { return picks[e] == CUSTOM ? customs[e].name.c_str() : soundName(picks[e]); }
bool hasCustom(Event e) { return e < EVENT_COUNT && customs[e].n; }
const char *customName(Event e) { return customs[e].name.c_str(); }
void setSound(Event e, int i) {
  if (e >= EVENT_COUNT || i < -3 || i == -2 || i >= SOUND_COUNT || (i == CUSTOM && !customs[e].n)) return;
  picks[e] = i;
  savePicks();
}
void playSound(int i, Event e) {
  if (i >= 0 && i < SOUND_COUNT) { start(&SOUNDS[i], false); return; }
  if (i == CUSTOM && e < EVENT_COUNT && customs[e].n) {
    const Custom &c = customs[e];
    Sound s{"*", c.name.c_str(), 0, c.steps, c.n, c.repeat, c.gap, c.soft, false};
    start(&s, isClick(e));
    return;
  }
  buzzer::stop();
}
const char *soundName(int i) { return i >= 0 && i < SOUND_COUNT ? SOUNDS[i].name : "No sound"; }
int changedCount() {
  int n = 0;
  for (int e = 0; e < EVENT_COUNT; e++) n += picks[e] != EVENTS[e].def;
  return n;
}
void resetChoices() {
  for (int e = 0; e < EVENT_COUNT; e++) {
    picks[e] = EVENTS[e].def;
    if (customs[e].n) { dropCustom(e); store::remove(customKey(e).c_str()); }
  }
  savePicks();
}

void serialCommand(const char *s) {
  auto ok = []() { Serial.println("[sound] ok"); };
  auto err = [](const char *why) { Serial.printf("[sound] error %s\n", why); };
  auto eventOf = [](const String &id) -> int {
    for (int e = 0; e < EVENT_COUNT; e++)
      if (id == EVENTS[e].id) return e;
    return -1;
  };
  char cmd = *s ? *s++ : 0;
  while (*s == ' ') s++;
  String rest(s);
  if (cmd == '?') {
    Serial.printf("[sound] vol %d\n", vol);
    Serial.printf("[sound] clicks %d\n", clicksOn);
    for (int e = 0; e < EVENT_COUNT; e++) {
      const char *id = picks[e] == CUSTOM ? "*" : picks[e] < 0 ? "-" : SOUNDS[picks[e]].id;
      Serial.printf("[sound] pick %s %s\n", EVENTS[e].id, id);
      if (customs[e].n) Serial.printf("[sound] custom %s %s\n", EVENTS[e].id, store::getStr(customKey(e).c_str()).c_str());
    }
    Serial.println("[sound] end");
  } else if (cmd == 'P' || cmd == 'C') {
    int sp = rest.indexOf(' ');
    int e = eventOf(sp > 0 ? rest.substring(0, sp) : rest);
    if (e < 0) { err("unknown event"); return; }
    String arg = sp > 0 ? rest.substring(sp + 1) : String("");
    if (cmd == 'P') {
      int i = findSound(arg);
      if (i == -2 || (i == CUSTOM && !customs[e].n)) { err("unknown sound"); return; }
      setSound((Event)e, i);
    } else {
      if ((int)arg.length() > CUSTOM_MAX_TEXT) { err("too long"); return; }
      Custom c;
      if (!parseCustom(arg, c)) { err("bad sound"); return; }
      dropCustom(e);
      customs[e] = c;
      store::setStr(customKey(e).c_str(), arg);
      setSound((Event)e, CUSTOM);
    }
    ok();
  } else if (cmd == 'V') {
    setVolume((Volume)rest.toInt());
    ok();
  } else if (cmd == 'K') {
    setClicks(rest.toInt() != 0);
    ok();
  } else if (cmd == 'R') {
    resetChoices();
    ok();
  } else {
    err("unknown command");
  }
}

}  // namespace sounds
