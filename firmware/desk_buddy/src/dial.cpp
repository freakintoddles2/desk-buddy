#include "dial.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <math.h>

#include "theme.h"

namespace {

constexpr float U = 4.8f;            // display.html viewBox unit in px
constexpr float R = 46.9f * U;       // track / arc centerline
constexpr float W = 2.6f * U;        // track and arc width (a touch heavier than 2.4)
constexpr int TICKS = 60;

lv_point_precise_t polar(float deg, float r) {
  float a = deg * M_PI / 180;
  return {240 + r * sinf(a), 240 - r * cosf(a)};
}

// The dial is drawn once into this full-screen RGB565 image (shared: one dial is on screen at a
// time) and the image is what LVGL shows. Drawing the arcs and ticks directly cost ~7.5 ms per
// 48-row strip, ~75 ms a frame, which made swiping into Focus stutter; copying the image is cheap.
uint16_t *cacheBuf = nullptr;
float cacheF = -1;
bool cacheAmber = false;

}  // namespace

void Dial::releaseCache() {
  heap_caps_free(cacheBuf);
  cacheBuf = nullptr;
  cacheF = -1;
}

void Dial::build(lv_obj_t *root) {
  if (!cacheBuf) cacheBuf = (uint16_t *)heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);
  if (!cacheBuf) {  // PSRAM ran out: no ring; the time and controls still work
    Serial.println("[dial] no room for the ring image");
    obj = nullptr;
    return;
  }
  obj = lv_canvas_create(root);
  lv_canvas_set_buffer(obj, cacheBuf, 480, 480, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(obj, 0, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  last = millis();
  fresh = true;
  // The image is kept between visits: it's redrawn only if the value or color differs from what
  // it last showed (whichever dial drew it), so re-entering Focus costs nothing extra.
}

// The screen rectangle covering the ring between two fractions (arc, glow, ticks, head dot).
lv_area_t ringSpan(float f0, float f1) {
  float a = fminf(f0, f1) * 360, b = fmaxf(f0, f1) * 360;
  float x1 = 480, y1 = 480, x2 = 0, y2 = 0;
  for (float d = a;; d += 2) {
    float deg = fminf(d, b);
    for (float r : {188.0f, 254.0f}) {
      lv_point_precise_t p = polar(deg, r);
      x1 = fminf(x1, p.x), y1 = fminf(y1, p.y), x2 = fmaxf(x2, p.x), y2 = fmaxf(y2, p.y);
    }
    if (deg >= b) break;
  }
  // 22 px margin: the head dot (radius 16) and the glow's round caps reach past the centerline samples.
  return {(int32_t)fmaxf(0, x1 - 22), (int32_t)fmaxf(0, y1 - 22), (int32_t)fminf(479, x2 + 22), (int32_t)fminf(479, y2 + 22)};
}

// Redraw the image if it doesn't show this dial's current state. Runs outside LVGL's own
// rendering (from set/tick), since it draws through a canvas layer of its own. When only the
// value moved, just the slice of ring between the old and new value is redrawn and sent to the
// screen (a full-screen redraw per knob step made turning feel heavy).
void Dial::render() {
  if (!obj || (shown == cacheF && amber == cacheAmber)) return;
  bool partial = cacheF >= 0 && amber == cacheAmber;
  lv_area_t area = partial ? ringSpan(cacheF, shown) : lv_area_t{0, 0, 479, 479};
  cacheF = shown;
  cacheAmber = amber;

  lv_layer_t layer;
  lv_canvas_init_layer(obj, &layer);
  layer._clip_area = area;
  layer.phy_clip_area = area;
  lv_draw_rect_dsc_t bg;
  lv_draw_rect_dsc_init(&bg);
  bg.bg_color = theme::screen();
  bg.bg_opa = LV_OPA_COVER;
  lv_draw_rect(&layer, &bg, &area);
  paint(&layer, shown, amber);
  // finish_layer invalidates the whole canvas; hold that off and invalidate just the slice.
  // LVGL counts enable/disable calls (inv_en_cnt), so these must stay a balanced pair: while
  // the tray has invalidation off, the count stays below zero and the invalidate is ignored.
  lv_display_t *disp = lv_obj_get_display(obj);
  lv_display_enable_invalidation(disp, false);
  lv_canvas_finish_layer(obj, &layer);
  lv_display_enable_invalidation(disp, true);
  lv_obj_invalidate_area(obj, &area);
}

void Dial::set(float frac, bool amb) {
  target = fmaxf(0, fminf(1, frac));
  // The first value after a build appears at once: easing up from empty while the panel is
  // sliding in redraws the whole screen for ~250 ms and makes the swipe stutter.
  bool jump = !smooth || fresh;
  fresh = false;
  if (jump) shown = target;
  if (amb != amber || jump) {
    amber = amb;
    render();
  }
}

void Dial::tick() {
  uint32_t now = millis();
  float dt = (float)(now - last);
  last = now;
  if (!obj || fabsf(target - shown) < 0.0005f) return;
  // Ease toward the target (about 220 ms, like the faux display's "snappy" transition).
  shown += (target - shown) * (1 - expf(-dt / 70.0f));
  if (fabsf(target - shown) < 0.0005f) shown = target;
  render();
}

void Dial::paint(lv_layer_t *layer, float f, bool amberColor) {
  const lv_color_t color = amberColor ? theme::today() : theme::accent();

  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.center.x = 240;
  arc.center.y = 240;

  // Track.
  arc.radius = (uint16_t)(R + W / 2);
  arc.width = (int32_t)W;
  arc.start_angle = 0;
  arc.end_angle = 360;
  arc.color = lv_color_white();
  arc.opa = 18;
  lv_draw_arc(layer, &arc);

  // Ticks.
  int lit = (int)lroundf(f * TICKS);
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.round_start = line.round_end = 1;
  line.color = lv_color_white();
  for (int i = 0; i < TICKS; i++) {
    bool major = i % 5 == 0;
    float deg = i * 6.0f;
    line.p1 = polar(deg, (major ? 40.4f : 41.8f) * U);
    line.p2 = polar(deg, 43.4f * U);
    line.width = major ? 3 : 2;
    bool on = i < lit;
    line.opa = major ? (on ? 217 : 56) : (on ? 128 : 31);
    lv_draw_line(layer, &line);
  }

  if (f <= 0.002f) return;

  // Progress arc with a soft glow (two wider, faint arcs underneath).
  float end = -90 + f * 360;
  arc.start_angle = -90 + 360;  // LVGL angles: 0 = 3 o'clock, clockwise; keep them positive
  arc.end_angle = end + 360;
  arc.color = color;
  arc.rounded = 1;
  for (int k = 0; k < 2; k++) {
    float gw = W + (k ? 14 : 7);
    arc.width = (int32_t)gw;
    arc.radius = (uint16_t)(R + gw / 2);
    arc.opa = k ? 25 : 45;
    lv_draw_arc(layer, &arc);
  }
  arc.width = (int32_t)W;
  arc.radius = (uint16_t)(R + W / 2);
  arc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &arc);

  // Head dot.
  lv_point_precise_t h = polar(f * 360, R);
  lv_draw_rect_dsc_t dot;
  lv_draw_rect_dsc_init(&dot);
  dot.radius = LV_RADIUS_CIRCLE;
  dot.bg_color = color;
  dot.bg_opa = 90;
  lv_area_t a = {(int32_t)(h.x - 16), (int32_t)(h.y - 16), (int32_t)(h.x + 16), (int32_t)(h.y + 16)};
  lv_draw_rect(layer, &dot, &a);
  dot.bg_color = lv_color_white();
  dot.bg_opa = LV_OPA_COVER;
  a = {(int32_t)(h.x - 10), (int32_t)(h.y - 10), (int32_t)(h.x + 10), (int32_t)(h.y + 10)};
  lv_draw_rect(layer, &dot, &a);
}
