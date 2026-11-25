// Habits and Days panels (LISTS.habits / LISTS.countdowns and their detail screens in
// display.html). Both are scroll-wheel lists ending in "+ New ..."; picking an item opens its
// detail screen, whose chips run Back before Remove.
#include "daynum.h"
#include "icons.h"
#include "list_wheel.h"
#include "modes.h"
#include "records.h"
#include "theme.h"
#include "sounds.h"

LV_FONT_DECLARE(font_clock_96);

namespace {

// Toggle a habit's day; a check-off (not an un-check) gets the check sound.
void habitToggled(uint32_t id, int32_t day) {
  habits::toggle(id, day);
  habits::Habit *h = habits::find(id);
  if (h && habits::isDone(*h, day)) sounds::play(sounds::HABIT_DONE);
}

const char *SEP = " \xE2\x80\xA2 ";  // " • "

// Set a label's text only when it differs (setting it always redraws the label).
void setText(lv_obj_t *l, const String &t) {
  if (strcmp(lv_label_get_text(l), t.c_str())) lv_label_set_text(l, t.c_str());
}

// The list to select when its panel next builds (a just-added item).
String habitSel, countdownSel;

// ======================= habits =======================

String streakText(int s) { return s ? String(s) + "-day streak" : String("No streak yet"); }

struct HabitDetail : DetailMode {
  uint32_t id;

  explicit HabitDetail(uint32_t id) : id(id) {
    topText = "Habit";
    gap = 12;
    shiftY = 22;
    body = [this]() -> std::vector<Line> {
      habits::Habit *h = habits::find(this->id);
      return {{h ? h->name : String(""), &lv_font_montserrat_34, theme::ink()}};
    };
    extra = [this](lv_obj_t *col) { week(col); };
    controls = [this]() -> std::vector<Control> {
      habits::Habit *h = habits::find(this->id);
      if (!h) return {};
      bool done = habits::isDone(*h, daynum::today());
      uint32_t hid = this->id;
      return {
        {done ? String(LV_SYMBOL_OK " Done today") : String("Mark done"), done ? ui::Tone::Done : ui::Tone::Primary,
         [hid]() { habitToggled(hid, daynum::today()); app::refresh(); }},
        {"Back", ui::Tone::Normal, []() { app::pop(); }},
        {"Remove", ui::Tone::Stop, [this, hid]() {
          habits::remove(hid);
          ui::toast("Habit removed");
          app::drop(this);
        }},
      };
    };
  }

  // The last 7 days as circles, today on the right; tap one to check it off or clear it.
  void week(lv_obj_t *col) {
    habits::Habit *h = habits::find(id);
    if (!h) return;
    lv_obj_t *r = lv_obj_create(col);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_style_pad_all(r, 6, 0);  // room for today's outer ring
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    int32_t t = daynum::today();
    for (int i = 6; i >= 0; i--) {
      int32_t day = t - i;
      bool done = habits::isDone(*h, day);
      lv_obj_t *c = lv_obj_create(r);
      lv_obj_remove_style_all(c);
      lv_obj_set_size(c, 42, 42);
      lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(c, 2, 0);
      lv_obj_set_style_border_color(c, done ? theme::ok() : theme::inkFaint(), 0);
      lv_obj_set_style_bg_color(c, theme::ok(), 0);
      lv_obj_set_style_bg_opa(c, done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
      if (i == 0) {  // ".day.today": accent ring with a gap
        lv_obj_set_style_outline_width(c, 2, 0);
        lv_obj_set_style_outline_pad(c, 2, 0);
        lv_obj_set_style_outline_color(c, theme::accent(), 0);
      }
      lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(c, 4);
      lv_obj_set_user_data(c, (void *)(intptr_t)day);
      lv_obj_add_event_cb(c, [](lv_event_t *e) {
        auto *self = (HabitDetail *)lv_event_get_user_data(e);
        int32_t d = (int32_t)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
        habitToggled(self->id, d);
        app::refresh();
      }, LV_EVENT_CLICKED, this);
      char letter[2] = {daynum::dayShort(daynum::weekday(day))[0], 0};
      lv_obj_t *l = ui::label(c, letter, &lv_font_montserrat_18, done ? theme::screen() : theme::inkDim());
      lv_obj_center(l);
    }
    int s = habits::streak(*h);
    ui::label(col, streakText(s).c_str(), &lv_font_montserrat_22, s ? theme::ok() : theme::inkDim());
  }

  void build(lv_obj_t *root) override {
    if (!habits::find(id)) { app::drop(this); return; }
    DetailMode::build(root);
  }
};

// Custom first. "..." not "\u2026": the built-in fonts have no ellipsis glyph.
const char *HABIT_PRESETS[] = {"Custom...", "Drink water", "Stretch", "Walk", "Read", "Meditate", "Journal",
                               "No phone at lunch"};
constexpr int HABIT_CUSTOM = 0;

void addHabit(const String &name) {
  uint32_t id = habits::add(name);
  if (!id) { ui::toast("That's the most habits it can hold", -1); return; }
  habitSel = String(id);
  ui::toast((String("Added ") + name).c_str(), 1);
}

void newHabitFlow() {
  if ((int)habits::all().size() >= habits::MAX) { ui::toast("That's the most habits it can hold", -1); return; }
  std::vector<String> opts;
  for (auto *p : HABIT_PRESETS) opts.push_back(p);
  app::push(new PickerMode("New habit", "Pick one", opts, -1, [](int i) {
    if (i == HABIT_CUSTOM) app::push(new TextMode("Habit name", "", addHabit));
    else addHabit(HABIT_PRESETS[i]);
  }));
}

struct HabitsPanel : Panel {
  ListWheel wheel;
  lv_obj_t *topLbl = nullptr, *counter = nullptr;
  String selId;
  uint32_t lastRefresh = 0;

  HabitsPanel() : Panel("habits", "Habits", &icon_habits) {}

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    int32_t t = daynum::today();
    for (auto &h : habits::all())
      v.push_back({String(h.id), h.name, String(habits::doneInLast7(h)) + "/7", streakText(habits::streak(h)),
                   habits::isDone(h, t) ? ListItem::Ok : ListItem::Normal});
    v.push_back({"add", "+ New habit", "", "", ListItem::Add});
    return v;
  }

  void build(lv_obj_t *root) override {
    if (habitSel.length()) { selId = habitSel; habitSel = ""; }
    wheel = ListWheel();
    wheel.build(root);
    std::vector<ListItem> its = items();
    int start = 0;
    for (int i = 0; i < (int)its.size(); i++)
      if (its[i].id == selId) start = i;
    wheel.setItems(its);
    wheel.jump(start);
    wheel.onPick = [this](int) { pick(); };
    wheel.onChange = [this]() { remember(); };
    topLbl = ui::top(root, "");
    counter = ui::bottom(root, "");
    lv_obj_add_flag(counter, LV_OBJ_FLAG_HIDDEN);  // the scroll arc and the cut-off row replace "N / M"
    remember();
  }

  void remember() {
    if (const ListItem *it = wheel.selectedItem()) selId = it->id;
    int n = habits::all().size();
    setText(counter, String(wheel.selected() + 1) + " / " + (n + 1));
    setText(topLbl, n ? String(habits::doneToday()) + " OF " + n + " TODAY" : String("NO HABITS YET"));
  }

  void pick() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "add") newHabitFlow();
    else app::push(new HabitDetail(it->id.toInt()));
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { pick(); }
  void tick() override {
    wheel.tick();
    if (millis() - lastRefresh >= 1000) {  // a new day clears the green marks
      lastRefresh = millis();
      wheel.setItems(items());
      remember();
    }
  }
};

// ======================= days (countdowns) =======================

// Month / day / year fields for a countdown's date, starting at `day`.
void askDate(const String &title, int32_t day, std::function<void(int32_t)> onDone) {
  daynum::Ymd a = daynum::toYmd(day), now = daynum::toYmd(daynum::today());
  std::vector<Field> f = {
    {"Month", a.m, 1, 12, nullptr, [](int v) { return String(daynum::monthShort(v)); }},
    {"Day", a.d, 1, 31, [](const std::vector<Field> &f) { return daynum::daysInMonth(f[2].value, f[0].value); }, nullptr, 5},
    {"Year", a.y, min(a.y, now.y - 1), max(a.y, now.y + 10), nullptr, nullptr},
  };
  app::push(new FieldsMode(title, f,
    [](const std::vector<Field> &f) {
      int32_t z = daynum::fromYmd(f[2].value, f[0].value, f[1].value);
      return daynum::longText(z) + "\n" + countdowns::badge(z - daynum::today());
    },
    [onDone](const std::vector<Field> &f) { onDone(daynum::fromYmd(f[2].value, f[0].value, f[1].value)); },
    [](std::vector<Field> &f) {
      int dim = daynum::daysInMonth(f[2].value, f[0].value);
      if (f[1].value > dim) f[1].value = dim;
    }));
}

struct CountdownDetail : DetailMode {
  uint32_t id;

  explicit CountdownDetail(uint32_t id) : id(id) {
    gap = 2;
    body = [this]() -> std::vector<Line> {
      countdowns::Countdown *c = countdowns::find(this->id);
      if (!c) return {};
      int32_t n = countdowns::daysUntil(*c);
      std::vector<Line> v;
      if (n == 0) {
        v.push_back({"Today", &lv_font_montserrat_48, theme::ok()});
      } else {
        v.push_back({String(abs(n)), &font_clock_96, n < 0 ? theme::inkDim() : theme::ink()});
        v.push_back({n > 0 ? (n == 1 ? "day until" : "days until") : (n == -1 ? "day since" : "days since"),
                     &lv_font_montserrat_22, theme::inkDim()});
      }
      v.push_back({c->name, &lv_font_montserrat_30, theme::ink()});
      v.push_back({daynum::longText(c->day), &lv_font_montserrat_18, theme::inkDim()});
      return v;
    };
    controls = [this]() -> std::vector<Control> {
      countdowns::Countdown *c = countdowns::find(this->id);
      if (!c) return {};
      uint32_t cid = this->id;
      return {
        {"Change date", ui::Tone::Normal, [cid]() {
          countdowns::Countdown *x = countdowns::find(cid);
          if (x) askDate(x->name, x->day, [cid](int32_t d) { countdowns::setDay(cid, d); });
        }},
        {"Rename", ui::Tone::Normal, [cid]() {
          countdowns::Countdown *x = countdowns::find(cid);
          if (x) app::push(new TextMode("Rename", x->name, [cid](const String &t) { countdowns::rename(cid, t); }));
        }},
        {"Back", ui::Tone::Normal, []() { app::pop(); }},
        {"Remove", ui::Tone::Stop, [this, cid]() {
          countdowns::remove(cid);
          ui::toast("Countdown removed");
          app::drop(this);
        }},
      };
    };
  }

  void build(lv_obj_t *root) override {
    if (!countdowns::find(id)) { app::drop(this); return; }
    DetailMode::build(root);
  }
};

const char *CD_PRESETS[] = {"Custom...", "Trip", "Birthday", "Launch day", "Weekend", "Holiday break"};
constexpr int CD_CUSTOM = 0, CD_WEEKEND = 4;

void addCountdown(const String &name, int32_t day) {
  uint32_t id = countdowns::add(name, day);
  if (!id) { ui::toast("That's the most countdowns it can hold", -1); return; }
  countdownSel = String(id);
  ui::toast((name + SEP + countdowns::badge(day - daynum::today())).c_str(), 1);
}

void askCountdownDate(const String &name) {
  askDate(name, daynum::today() + 7, [name](int32_t d) { addCountdown(name, d); });
}

void newCountdownFlow() {
  if ((int)countdowns::all().size() >= countdowns::MAX) { ui::toast("That's the most countdowns it can hold", -1); return; }
  std::vector<String> opts;
  for (auto *p : CD_PRESETS) opts.push_back(p);
  app::push(new PickerMode("New countdown", "Counting down to", opts, -1, [](int i) {
    if (i == CD_WEEKEND) {
      int32_t t = daynum::today();
      int ahead = (6 - daynum::weekday(t) + 7) % 7;
      addCountdown("Weekend", t + (ahead ? ahead : 7));  // next Saturday
    } else if (i == CD_CUSTOM) {
      app::push(new TextMode("Countdown name", "", askCountdownDate));
    } else {
      askCountdownDate(CD_PRESETS[i]);
    }
  }));
}

struct DaysPanel : Panel {
  ListWheel wheel;
  lv_obj_t *topLbl = nullptr, *counter = nullptr;
  String selId;
  uint32_t lastRefresh = 0;

  DaysPanel() : Panel("countdowns", "Days", &icon_countdowns) {}

  std::vector<ListItem> items() {
    std::vector<ListItem> v;
    for (auto &c : countdowns::sorted()) {
      int32_t n = countdowns::daysUntil(c);
      v.push_back({String(c.id), c.name, countdowns::badge(n), daynum::shortText(c.day),
                   n < 0 ? ListItem::Past : n == 0 ? ListItem::Ok : ListItem::Normal});
    }
    v.push_back({"add", "+ New countdown", "", "", ListItem::Add});
    return v;
  }

  void build(lv_obj_t *root) override {
    if (countdownSel.length()) { selId = countdownSel; countdownSel = ""; }
    wheel = ListWheel();
    wheel.build(root);
    std::vector<ListItem> its = items();
    int start = 0;
    for (int i = 0; i < (int)its.size(); i++)
      if (its[i].id == selId) start = i;
    wheel.setItems(its);
    wheel.jump(start);
    wheel.onPick = [this](int) { pick(); };
    wheel.onChange = [this]() { remember(); };
    topLbl = ui::top(root, "");
    counter = ui::bottom(root, "");
    lv_obj_add_flag(counter, LV_OBJ_FLAG_HIDDEN);  // the scroll arc and the cut-off row replace "N / M"
    remember();
  }

  void remember() {
    if (const ListItem *it = wheel.selectedItem()) selId = it->id;
    setText(counter, String(wheel.selected() + 1) + " / " + ((int)countdowns::all().size() + 1));
    setText(topLbl, String(countdowns::upcomingCount()) + " COMING UP");
  }

  void pick() {
    const ListItem *it = wheel.selectedItem();
    if (!it) return;
    if (it->id == "add") newCountdownFlow();
    else app::push(new CountdownDetail(it->id.toInt()));
  }

  void rotate(int dir) override { wheel.rotate(dir); }
  void press() override { pick(); }
  void tick() override {
    wheel.tick();
    if (millis() - lastRefresh >= 1000) {  // badges change at midnight
      lastRefresh = millis();
      wheel.setItems(items());
      remember();
    }
  }
};

}  // namespace

Panel *makeHabitsPanel() { return new HabitsPanel(); }
Panel *makeDaysPanel() { return new DaysPanel(); }
