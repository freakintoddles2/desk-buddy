// CrowPanel 2.1" hardware. Pins and power-up sequence from Elecrow's Board21.h and factory demo.
#include "board.h"

#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_CST8XX.h>
#include <PCF8574.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <esp32s3/rom/cache.h>
#include <atomic>
#include "buzzer.h"

namespace board {
namespace {

constexpr int I2C_SDA = 38;
constexpr int I2C_SCL = 39;
constexpr int BACKLIGHT_PIN = 6;
constexpr int LED_PIN = 43;
constexpr int ENC_A = 42;
constexpr int ENC_B = 4;
constexpr uint8_t TOUCH_ADDR = 0x15;
// LVGL draws into these strips, then they're copied into the panel's framebuffer (PSRAM).
// Internal SRAM is several times faster to draw into than PSRAM. 48 lines = 46 KB each.
constexpr size_t BUF_LINES = 48;

// PCF8574: P0 touch reset, P2 touch int, P3 LCD power, P4 LCD reset, P5 knob switch (active low).
PCF8574 pcf(0x21);
Adafruit_CST8XX touch;

Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED, 16, 2, 1, GFX_NOT_DEFINED);
Arduino_ESP32RGBPanel *rgb = new Arduino_ESP32RGBPanel(
  40, 7, 15, 41, 46, 3, 8, 18, 17, 14, 13, 12, 11, 10, 9,
  5, 45, 48, 47, 21, 1, 10, 4, 20, 1, 10, 4, 20, 0, 12000000,
  false, 0, 0, 480 * 20);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  SCREEN_W, SCREEN_H, rgb, 0, true, bus, GFX_NOT_DEFINED,
  st7701_type5_init_operations, sizeof(st7701_type5_init_operations));

lv_indev_t *touchDev = nullptr;

// ---- knob: quadrature decoded in an interrupt ----
const int8_t QUAD[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
volatile uint8_t encState = 0;
volatile int8_t encSub = 0;
volatile int32_t encSteps = 0;
int32_t encTaken = 0;

void IRAM_ATTR encoderIsr() {
  uint8_t s = (gpio_get_level((gpio_num_t)ENC_A) << 1) | gpio_get_level((gpio_num_t)ENC_B);
  encSub = encSub + QUAD[(encState << 2) | s];
  encState = s;
  // The knob is a half-step encoder: it rests at both 00 and 11, so one detent is 2 transitions
  // (00 -> 01/10 -> 11, then 11 -> 10/01 -> 00 for the next). Count a step at each resting state,
  // and drop any leftover from contact bounce there. A/B are wired so that +2 is counter-clockwise,
  // hence the flipped sign.
  if (s == 0b00 || s == 0b11) {
    if (encSub >= 2) encSteps = encSteps - 1;
    else if (encSub <= -2) encSteps = encSteps + 1;
    encSub = 0;
  }
}


// ---- knob switch ----
uint8_t btnRaw = HIGH, btnStable = HIGH;
uint32_t btnChangedAt = 0, btnDownAt = 0, lastPoll = 0;
bool longFired = false, homeFired = false;

void powerOn() {
  Wire.begin(I2C_SDA, I2C_SCL);
  pcf.pinMode(P0, OUTPUT);
  pcf.pinMode(P2, OUTPUT);
  pcf.pinMode(P3, OUTPUT);
  pcf.pinMode(P4, OUTPUT);
  pcf.pinMode(P5, INPUT_PULLUP);
  if (!pcf.begin()) Serial.println("[board] PCF8574 not found");

  pcf.digitalWrite(P3, HIGH);
  delay(50);
  pcf.digitalWrite(P4, LOW);
  delay(100);
  pcf.digitalWrite(P4, HIGH);
  delay(100);
  pcf.digitalWrite(P0, LOW);
  delay(100);
  pcf.digitalWrite(P0, HIGH);
  delay(100);
  pcf.digitalWrite(P2, HIGH);

  ledcAttach(BACKLIGHT_PIN, 5000, 12);  // 12-bit: smooth slow fades near 5% (auto brightness)
  ledcWrite(BACKLIGHT_PIN, 0);  // stay dark until the first frame is drawn
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

void panelOn() {
  // The RGB panel refreshes from PSRAM through bounce buffers that an interrupt refills ~43
  // times a second. Interrupts run on the core that installed them, so start the panel from a
  // task on core 0 (Wi-Fi, painter): that keeps the copying off core 1, where LVGL draws.
  static volatile int gfxState = 0;  // 0 running, 1 ok, -1 failed
#ifndef PANEL_CORE
#define PANEL_CORE 0
#endif
  BaseType_t ok = xTaskCreatePinnedToCore([](void *) {
    gfxState = gfx->begin() ? 1 : -1;
    vTaskDelete(nullptr);
  }, "panel", 6144, nullptr, 5, nullptr, PANEL_CORE);
  if (ok != pdPASS) {  // no task: start it here (the panel's copying then runs on this core)
    Serial.println("[board] no panel task: starting the display on the UI core");
    gfxState = gfx->begin() ? 1 : -1;
  }
  while (!gfxState) delay(1);
  if (gfxState < 0) Serial.println("[board] display begin failed");
  bus->beginWrite();
  // MADCTL. Elecrow's code writes 0x08 (BGR order), but on this panel that swaps red and
  // blue (photo: the blue HOME pill showed orange). 0x00 = RGB order.
  bus->writeCommand(0x36);
  bus->write(0x00);
  bus->endWrite();
  gfx->fillScreen(RGB565_BLACK);
  if (!touch.begin(&Wire, TOUCH_ADDR)) Serial.println("[board] touch controller not found");
}

// Frame timing (while building): once a second while anything redraws, logs
// "[perf] N fps, render avg/max ms, flush ms, px" on serial. Set to 0 to remove.
#define PERF_LOG 0  // 1: print frame timing over serial while anything redraws (docs/testing.md)
#if PERF_LOG
uint32_t perfStart = 0, perfFrames = 0, perfRenderUs = 0, perfRenderMax = 0, perfFlushUs = 0, perfPx = 0, perfWindow = 0;
void perfEvent(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
    perfStart = micros();
    return;
  }
  uint32_t us = micros() - perfStart;
  perfFrames++;
  perfRenderUs += us;
  perfRenderMax = max(perfRenderMax, us);
  if (!perfWindow) perfWindow = millis();
  if (millis() - perfWindow >= 1000) {
    Serial.printf("[perf] %lu fps, frame avg %.1f max %.1f ms, flush %.1f ms/frame, %lu kpx/frame\n",
                  (unsigned long)perfFrames, perfRenderUs / 1000.0f / perfFrames, perfRenderMax / 1000.0f,
                  perfFlushUs / 1000.0f / perfFrames, (unsigned long)(perfPx / perfFrames / 1000));
    perfFrames = perfRenderUs = perfRenderMax = perfFlushUs = perfPx = 0;
    perfWindow = 0;
  }
}
#endif

// Flushing = copying a finished strip into the panel's framebuffer (PSRAM) and writing the
// cache back. It runs on a core-0 task so LVGL can draw the next strip into the other buffer
// at the same time; the task tells LVGL when the buffer is free again.
struct FlushJob {
  lv_display_t *d;
  lv_area_t a;
  uint8_t *px;
};
QueueHandle_t flushQ = nullptr;
bool flushInline = false;  // no flush task: draw in flushCb
std::atomic<int> flushBusy{0};

void flushTask(void *) {
  FlushJob j;
  for (;;) {
    if (xQueueReceive(flushQ, &j, portMAX_DELAY) != pdTRUE) continue;
#if PERF_LOG
    uint32_t t0 = micros();
#endif
    gfx->draw16bitRGBBitmap(j.a.x1, j.a.y1, (uint16_t *)j.px, j.a.x2 - j.a.x1 + 1, j.a.y2 - j.a.y1 + 1);
#if PERF_LOG
    perfFlushUs += micros() - t0;
    perfPx += (j.a.x2 - j.a.x1 + 1) * (j.a.y2 - j.a.y1 + 1);
#endif
    lv_display_flush_ready(j.d);
    flushBusy--;
  }
}

void flushCb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  if (flushInline) {
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
    lv_display_flush_ready(d);
    return;
  }
  FlushJob j{d, *a, px};
  flushBusy++;
  xQueueSend(flushQ, &j, portMAX_DELAY);
}

// Tap debugging (while building): a dot where the touch controller says the finger is, kept
// on screen for 1.5 s after release, plus the down/up points on serial. Set to 0 to remove.
#define TOUCH_DEBUG 0  // off 2026-11-09 (calibration done)
#if TOUCH_DEBUG
lv_obj_t *touchDot = nullptr;
bool touchWasDown = false;
uint32_t touchUpAt = 0;
int32_t touchX = 0, touchY = 0;

void touchDebug(bool down, int32_t x, int32_t y) {
  if (!touchDot) {
    touchDot = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(touchDot);
    lv_obj_set_size(touchDot, 20, 20);
    lv_obj_set_style_radius(touchDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touchDot, lv_color_hex(0xFF3B6B), 0);
    lv_obj_set_style_bg_opa(touchDot, LV_OPA_80, 0);
    lv_obj_set_style_border_color(touchDot, lv_color_white(), 0);
    lv_obj_set_style_border_width(touchDot, 2, 0);
    lv_obj_remove_flag(touchDot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
  }
  if (down) {
    if (!touchWasDown) Serial.printf("[touch] down %ld,%ld\n", (long)x, (long)y);
    touchWasDown = true;
    touchX = x;
    touchY = y;
    lv_obj_set_pos(touchDot, x - 10, y - 10);
    lv_obj_remove_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
  } else if (touchWasDown) {
    Serial.printf("[touch] up %ld,%ld\n", (long)touchX, (long)touchY);
    touchWasDown = false;
    touchUpAt = millis();
  } else if (millis() - touchUpAt > 1500) {
    lv_obj_add_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
  }
}
#endif

// Taps land a little below where people aim (finger pad below the fingertip, plus looking down at
// the screen through the cover glass), so every touch is moved up this many pixels.
// Tune by eye with the TOUCH_DEBUG dot; positive = up.
#ifndef TOUCH_Y_SHIFT
#define TOUCH_Y_SHIFT 10
#endif

bool (*touchHook)(bool down, int x, int y) = nullptr;
int16_t startX = 0, startY = 0;
bool wasPressed = false;

bool readTouchRaw(int16_t &x, int16_t &y) {
  if (!touch.touched()) return false;
  CST_TS_Point p = touch.getPoint(0);
  // On lift-off the chip can report one last point at (0, 0) before it says the finger is up
  // (seen 2026-11-24: it dragged the fast travel tray one panel over on a still tap). That
  // corner is outside the round glass, so it's never a real touch: treat it as no touch.
  if (p.x == 0 && p.y == 0) return false;
  x = constrain(p.x, 0, SCREEN_W - 1);
  y = constrain((int32_t)p.y - TOUCH_Y_SHIFT, 0, SCREEN_H - 1);
  return true;
}

void touchReadCb(lv_indev_t *, lv_indev_data_t *data) {
  int16_t x, y;
  if (readTouchRaw(x, y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    if (!wasPressed) { startX = x; startY = y; }
    wasPressed = true;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    wasPressed = false;
  }
  // The fast travel tray takes over a pull from the top edge; LVGL then sees no touch.
  if (touchHook && touchHook(data->state == LV_INDEV_STATE_PRESSED, data->point.x, data->point.y)) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
#if TOUCH_DEBUG
  touchDebug(data->state == LV_INDEV_STATE_PRESSED, data->point.x, data->point.y);
#endif
}

void lvglOn() {
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  const size_t bytes = sizeof(uint16_t) * SCREEN_W * BUF_LINES;
  uint8_t *buf1 = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint8_t *buf2 = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf1 || !buf2) {  // not expected; PSRAM works, just slower
    Serial.println("[board] draw buffers in PSRAM (internal RAM short)");
    if (!buf1) buf1 = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf2) buf2 = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  }
  if (!buf1 || !buf2) Serial.println("[board] LVGL buffer allocation failed");

  lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, buf1, buf2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  flushQ = xQueueCreate(2, sizeof(FlushJob));
  if (!flushQ || xTaskCreatePinnedToCore(flushTask, "flush", 4096, nullptr, 6, nullptr, 0) != pdPASS) {
    Serial.println("[board] no flush task: flushing on the UI core");
    flushInline = true;
  }
  lv_display_set_flush_cb(disp, flushCb);
#if PERF_LOG
  lv_display_add_event_cb(disp, perfEvent, LV_EVENT_RENDER_START, nullptr);
  lv_display_add_event_cb(disp, perfEvent, LV_EVENT_RENDER_READY, nullptr);
#endif

  touchDev = lv_indev_create();
  lv_indev_set_type(touchDev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touchDev, touchReadCb);
  lv_indev_set_display(touchDev, disp);
  lv_indev_set_long_press_time(touchDev, LONG_PRESS_MS);
}

}  // namespace

void begin() {
  powerOn();
  panelOn();
  lvglOn();

  pinMode(ENC_A, INPUT);
  pinMode(ENC_B, INPUT);
  encState = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderIsr, CHANGE);

  btnRaw = btnStable = pcf.digitalRead(P5, true);
  buzzer::begin();
}


int32_t injectedSteps = 0;
Button injected[2] = {Button::None, Button::None};  // a simulated press is Down, then Press

int32_t takeKnobSteps() {
  int32_t now = encSteps + injectedSteps;
  int32_t d = now - encTaken;
  encTaken = now;
  return d;
}

uint32_t downAt() { return btnDownAt; }

uint16_t *framebuffer() { return (uint16_t *)gfx->getFramebuffer(); }
void framebufferDone() { Cache_WriteBack_Addr((uint32_t)gfx->getFramebuffer(), 480 * 480 * 2); }
void framebufferDone(int y0, int y1) {
  if (y1 <= y0) return;
  Cache_WriteBack_Addr((uint32_t)gfx->getFramebuffer() + y0 * SCREEN_W * 2, (y1 - y0) * SCREEN_W * 2);
}

void setTouchHook(bool (*hook)(bool down, int x, int y)) { touchHook = hook; }
bool readTouch(int16_t &x, int16_t &y) { return readTouchRaw(x, y); }
void touchStart(int16_t &x, int16_t &y) { x = startX; y = startY; }
void waitFlushIdle() {
  while (flushBusy > 0) delay(1);
}

Button pollButton() {
  if (injected[0] != Button::None) {
    Button b = injected[0];
    injected[0] = injected[1];
    injected[1] = Button::None;
    if (b == Button::Down) btnDownAt = millis();
    return b;
  }
  uint32_t now = millis();
  if (now - lastPoll < 10) return Button::None;
  lastPoll = now;

  uint8_t s = pcf.digitalRead(P5, true);
  if (s != btnRaw) {
    btnRaw = s;
    btnChangedAt = now;
  }
  if (btnRaw != btnStable && now - btnChangedAt >= 30) {
    btnStable = btnRaw;
    if (btnStable == LOW) {
      btnDownAt = btnChangedAt;  // when the contact first closed, not after the debounce
      longFired = homeFired = false;
      return Button::Down;
    } else if (!longFired) {
      return Button::Press;
    }
  }
  if (btnStable == LOW && !longFired && now - btnDownAt >= LONG_PRESS_MS) {
    longFired = true;
    return Button::LongPress;
  }
  if (btnStable == LOW && longFired && !homeFired && now - btnDownAt >= HOME_HOLD_MS) {
    homeFired = true;
    return Button::HoldHome;
  }
  return Button::None;
}

void setBrightness(uint8_t pct) { setBacklight(pct); }

void setBacklight(float pct) {
  pct = constrain(pct, 0.0f, 100.0f);
  ledcWrite(BACKLIGHT_PIN, (uint32_t)lroundf(pct * 4095 / 100));
}

void setBacklightDuty(uint32_t duty) { ledcWrite(BACKLIGHT_PIN, min<uint32_t>(duty, 4095)); }

void injectKnob(int32_t steps) { injectedSteps += steps; }
void injectButton(Button b) {
  if (b == Button::Press) { injected[0] = Button::Down; injected[1] = Button::Press; }
  else { injected[0] = b; injected[1] = Button::None; }
}

void setLed(bool on) { digitalWrite(LED_PIN, on ? HIGH : LOW); }

lv_indev_t *touchIndev() { return touchDev; }

void sendScreenshot() {
  const uint8_t *fb = (const uint8_t *)gfx->getFramebuffer();
  const size_t total = (size_t)SCREEN_W * SCREEN_H * 2;
  Serial.flush();
  Serial.printf("SNAP %u %u\n", SCREEN_W, SCREEN_H);
  for (size_t off = 0; off < total;) {
    size_t n = Serial.write(fb + off, min((size_t)4096, total - off));
    if (n == 0) delay(1);
    off += n;
  }
  Serial.print("\nEND\n");
  Serial.flush();
}

}  // namespace board
