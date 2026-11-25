// Panel framework and input dispatch (mirrors display.html's panels, modes and input model):
//   rotate = move the selection / change the value being edited
//   press  = select / confirm the highlighted thing
//   swipe  = go to another panel (or leave a sub-screen)
//   tap    = act on the control touched
//   hold   = back out of a sub-screen
#pragma once

#include <lvgl.h>
#include <vector>

#include "ui.h"

// A panel or a sub-screen. build() creates widgets inside `root` (a transparent 480x480
// container) from the screen's own state; the app may destroy and rebuild it at any time.
struct Screen {
  virtual ~Screen() = default;
  virtual void build(lv_obj_t *root) = 0;
  virtual void rotate(int dir) {}
  virtual void press() {}
  virtual void tick() {}
  // Sub-screens only: return true if the swipe was used (e.g. dice); otherwise the app backs out.
  virtual bool swipe(int dir) { return false; }
  // Sub-screens only: ignore swipes entirely (games).
  virtual bool noSwipe() const { return false; }
  // A vertical swipe: dir -1 = up, +1 = down; startY is where the finger first touched.
  // Return true if the swipe was used; otherwise the touch carries on (a list's flick).
  virtual bool swipeVertical(int dir, int startY) { return false; }
  virtual bool isEntry() const { return false; }  // text or number entry: alerts wait underneath
  virtual bool isAlert() const { return false; }
  virtual bool isGame() const { return false; }
  virtual const char *tag() const { return nullptr; }
  // A finger is setting something (a dial ring): swipes and holds are ignored until it lifts.
  virtual bool ownsTouch() const { return false; }  // names a screen others look for
  // Something is running or being set up here (a timer, a breathing session): the idle
  // return to Home leaves this screen alone.
  virtual bool active() const { return false; }
  // The knob went down at `at` (millis). Press() still follows on release. For timing games.
  virtual void pressDown(uint32_t at) {}
};

struct Panel : Screen {
  const char *id;
  const char *name;
  const lv_image_dsc_t *icon;
  Panel(const char *id, const char *name, const lv_image_dsc_t *icon) : id(id), name(name), icon(icon) {}
  virtual bool visible() const { return true; }  // e.g. Weather hides itself without Wi-Fi
  // Called every loop while the panel is NOT on screen, for cheap upkeep such as starting a
  // background paint before the first visit. Must not touch LVGL objects.
  virtual void background() {}
};

namespace app {

void begin(const std::vector<Panel *> &panels);
void loop();

// Sub-screens stack on top of the current panel. The app owns pushed screens.
void push(Screen *mode);
void pop();
// Remove a specific sub-screen (e.g. a picker that just made its choice).
void drop(Screen *mode);
// Alerts go on top, except under a text/number entry in progress so typing isn't interrupted.
void pushAlert(Screen *alert);

// Rebuild the visible screen from its state (after a change that alters the layout).
void refresh();

// Jump to a panel by id (e.g. from Home's glance row).
void goTo(const char *id);

// All panels in the user's order (hidden ones included), for Settings > Panels.
std::vector<Panel *> orderedPanels();
// Panels shown in the swipe order.
std::vector<Panel *> shownPanels();
bool locked(const Panel *p);  // Home and Settings can't be hidden

// True while something shouldn't be interrupted (focus session, game, alert on screen).
bool busy();

// Go to Home from anywhere (closing sub-screens), unless an alert is waiting for an answer.
void goHome();
// The top screen is a sub-screen that can be left (not an alert): its title gets a back chevron.
bool canGoBack();
// The knob was used more recently than the touch screen. Lists show their outline (the knob's
// place) only then (2026-11-10).
bool knobActive();
void back();  // the title's chevron: leave the sub-screen (swipe and hold still work too)

// Return to Home after this many idle minutes (0 = never). Saved in NVS.
int idleMinutes();
void setIdleMinutes(int m);

// Fast travel tray (tray.cpp).
Panel *currentPanel();
// Switch to a panel right now (closing sub-screens), with no slide. The tray draws it itself.
void jump(Panel *p);
// Push a sub-screen and build it right away (the tray, which draws over LVGL, needs it built
// before the sheet slides off it).
void pushNow(Screen *mode);
Screen *topScreen();
// A pull from the top edge may open the tray: only on a top-level panel, never in a sub-screen.
bool fastTravelAllowed();
bool alertWaiting();

// Simulated swipe from USB serial (testing).
void injectSwipe(int dir);
void injectSwipeVertical(int dir, int startY);

}  // namespace app
