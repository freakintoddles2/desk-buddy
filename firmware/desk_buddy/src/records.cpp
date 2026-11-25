#include "records.h"

#include <algorithm>

#include "clock.h"
#include "daynum.h"
#include "store.h"

// Stored as one NVS string per list: records separated by \x1e, fields by \x1f (neither can
// be typed on the ring keyboard). A habit's check-offs are hex, newest day first.

namespace {

constexpr char REC = '\x1e', FLD = '\x1f';

std::vector<String> splitOn(const String &s, char sep) {
  std::vector<String> out;
  int start = 0;
  while (start <= (int)s.length()) {
    int i = s.indexOf(sep, start);
    if (i < 0) i = s.length();
    out.push_back(s.substring(start, i));
    start = i + 1;
  }
  return out;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

}  // namespace

// ---------------- habits ----------------
namespace habits {
namespace {

std::vector<Habit> list;
uint32_t nextId = 1;

// Move a habit's history so bit 0 is today (days in between count as not done). An anchor in
// the future (the clock was set ahead, then corrected) moves back, dropping marks on days that
// haven't happened; that's only done once the clock is trusted, never on build time.
void catchUp(Habit &h, int32_t today) {
  if (today > h.anchor) {
    int32_t d = today - h.anchor;
    if (d >= HISTORY) h.done.reset();
    else h.done <<= d;
    h.anchor = today;
  } else if (today < h.anchor && clockx::trusted()) {
    int32_t d = h.anchor - today;
    if (d >= HISTORY) h.done.reset();
    else h.done >>= d;
    h.anchor = today;
  }
}

void save() {
  String s;
  for (auto &h : list) {
    if (s.length()) s += REC;
    s += String(h.id) + FLD + h.name + FLD + h.anchor + FLD;
    // Trim trailing never-done days to keep the string short.
    int last = -1;
    for (int i = 0; i < HISTORY; i++)
      if (h.done[i]) last = i;
    for (int i = 0; i <= last; i += 4) {
      int v = 0;
      for (int b = 0; b < 4; b++)
        if (i + b < HISTORY && h.done[i + b]) v |= 1 << b;
      s += "0123456789abcdef"[v];
    }
  }
  store::setStr("habits", s);
  Serial.printf("[habits] saved %d (%d bytes)\n", (int)list.size(), (int)s.length());
}

}  // namespace

void begin() {
  list.clear();
  String s = store::getStr("habits");
  if (!s.length()) return;
  for (const String &rec : splitOn(s, REC)) {
    std::vector<String> f = splitOn(rec, FLD);
    if (f.size() < 4) continue;
    Habit h;
    h.id = f[0].toInt();
    h.name = f[1];
    h.anchor = f[2].toInt();
    for (int i = 0; i < (int)f[3].length(); i++) {
      int v = hexVal(f[3][i]);
      for (int b = 0; b < 4; b++)
        if ((v >> b & 1) && i * 4 + b < HISTORY) h.done.set(i * 4 + b);
    }
    nextId = max(nextId, h.id + 1);
    list.push_back(h);
  }
  Serial.printf("[habits] loaded %d\n", (int)list.size());
}

const std::vector<Habit> &all() { return list; }

Habit *find(uint32_t id) {
  for (auto &h : list)
    if (h.id == id) return &h;
  return nullptr;
}

uint32_t add(const String &name) {
  if ((int)list.size() >= MAX) return 0;
  Habit h;
  h.id = nextId++;
  h.name = name;
  h.anchor = daynum::today();
  list.push_back(h);
  save();
  return h.id;
}

void remove(uint32_t id) {
  list.erase(std::remove_if(list.begin(), list.end(), [id](const Habit &h) { return h.id == id; }), list.end());
  save();
}

bool isDone(const Habit &h, int32_t day) {
  int32_t i = h.anchor - day;
  return i >= 0 && i < HISTORY && h.done[i];
}

void rebase() {
  int32_t today = daynum::today();
  bool moved = false;
  for (auto &h : list)
    if (h.anchor > today) {
      catchUp(h, today);
      moved = true;
    }
  if (moved) {
    save();
    Serial.println("[habits] histories moved back to today (the clock had been ahead)");
  }
}

void toggle(uint32_t id, int32_t day) {
  Habit *h = find(id);
  if (!h) return;
  catchUp(*h, max(day, daynum::today()));
  int32_t i = h->anchor - day;
  if (i < 0 || i >= HISTORY) return;
  h->done.flip(i);
  save();
}

int streak(const Habit &h) {
  int32_t t = daynum::today();
  int32_t d = isDone(h, t) ? t : t - 1;
  int n = 0;
  while (isDone(h, d)) { n++; d--; }
  return n;
}

int doneInLast7(const Habit &h) {
  int32_t t = daynum::today();
  int n = 0;
  for (int i = 0; i < 7; i++) n += isDone(h, t - i);
  return n;
}

int doneToday() {
  int32_t t = daynum::today();
  int n = 0;
  for (auto &h : list) n += isDone(h, t);
  return n;
}

}  // namespace habits

// ---------------- countdowns ----------------
namespace countdowns {
namespace {

std::vector<Countdown> list;
uint32_t nextId = 1;

void save() {
  String s;
  for (auto &c : list) {
    if (s.length()) s += REC;
    s += String(c.id) + FLD + c.name + FLD + c.day;
  }
  store::setStr("cdowns", s);
  Serial.printf("[countdowns] saved %d\n", (int)list.size());
}

}  // namespace

void begin() {
  list.clear();
  String s = store::getStr("cdowns");
  if (!s.length()) return;
  for (const String &rec : splitOn(s, REC)) {
    std::vector<String> f = splitOn(rec, FLD);
    if (f.size() < 3) continue;
    Countdown c{(uint32_t)f[0].toInt(), f[1], (int32_t)f[2].toInt()};
    nextId = max(nextId, c.id + 1);
    list.push_back(c);
  }
  Serial.printf("[countdowns] loaded %d\n", (int)list.size());
}

const std::vector<Countdown> &all() { return list; }

Countdown *find(uint32_t id) {
  for (auto &c : list)
    if (c.id == id) return &c;
  return nullptr;
}

uint32_t add(const String &name, int32_t day) {
  if ((int)list.size() >= MAX) return 0;
  Countdown c{nextId++, name, day};
  list.push_back(c);
  save();
  return c.id;
}

void remove(uint32_t id) {
  list.erase(std::remove_if(list.begin(), list.end(), [id](const Countdown &c) { return c.id == id; }), list.end());
  save();
}

void rename(uint32_t id, const String &name) {
  if (Countdown *c = find(id)) { c->name = name; save(); }
}

void setDay(uint32_t id, int32_t day) {
  if (Countdown *c = find(id)) { c->day = day; save(); }
}

int32_t daysUntil(const Countdown &c) { return c.day - daynum::today(); }

std::vector<Countdown> sorted() {
  int32_t t = daynum::today();
  std::vector<Countdown> up, past;
  for (auto &c : list) (c.day >= t ? up : past).push_back(c);
  std::sort(up.begin(), up.end(), [](const Countdown &a, const Countdown &b) { return a.day < b.day; });
  std::sort(past.begin(), past.end(), [](const Countdown &a, const Countdown &b) { return a.day > b.day; });
  up.insert(up.end(), past.begin(), past.end());
  return up;
}

int upcomingCount() {
  int32_t t = daynum::today();
  int n = 0;
  for (auto &c : list) n += c.day >= t;
  return n;
}

String badge(int32_t n) {
  if (n == 0) return "Today";
  if (n == 1) return "Tomorrow";
  if (n > 0) return String(n) + " days";
  return String(-n) + "d ago";
}

}  // namespace countdowns
