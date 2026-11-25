// Desk buddy hardware test for the CrowPanel 2.1" rotary display.
// Checks the panel (color bars), LVGL rendering, touch, knob rotation, knob press,
// backlight PWM and the status LED. Every event is also logged on USB serial.
//
// Pins and the power-up sequence come from Elecrow's Board21.h (Arduino lessons)
// and the factory demo source.

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_CST8XX.h>
#include <PCF8574.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>

// ---------- Board configuration ----------
static constexpr uint16_t SCREEN_W = 480;
static constexpr uint16_t SCREEN_H = 480;
static constexpr int I2C_SDA = 38;
static constexpr int I2C_SCL = 39;
static constexpr int BACKLIGHT_PIN = 6;
static constexpr int LED_PIN = 43;
static constexpr int ENC_A = 42;
static constexpr int ENC_B = 4;
static constexpr uint8_t TOUCH_ADDR = 0x15;

// PCF8574 I/O expander: P0 touch reset, P2 touch int, P3 LCD power, P4 LCD reset, P5 knob switch.
static PCF8574 pcf(0x21);
static Adafruit_CST8XX touch;

static Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED, 16, 2, 1, GFX_NOT_DEFINED);
static Arduino_ESP32RGBPanel *rgb = new Arduino_ESP32RGBPanel(
  40, 7, 15, 41, 46, 3, 8, 18, 17, 14, 13, 12, 11, 10, 9,
  5, 45, 48, 47, 21, 1, 10, 4, 20, 1, 10, 4, 20, 0, 12000000,
  false, 0, 0, 480 * 20);
static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  SCREEN_W, SCREEN_H, rgb, 0, true, bus, GFX_NOT_DEFINED,
  st7701_type5_init_operations, sizeof(st7701_type5_init_operations));

// ---------- Knob (quadrature encoder, read in an interrupt) ----------
static const int8_t QUAD[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
static volatile uint8_t encState = 0;
static volatile int8_t encSub = 0;
static volatile int32_t encDetents = 0;

static void IRAM_ATTR encoderIsr() {
  uint8_t s = (gpio_get_level((gpio_num_t)ENC_A) << 1) | gpio_get_level((gpio_num_t)ENC_B);
  encSub += QUAD[(encState << 2) | s];
  encState = s;
  if (encSub >= 4) { encDetents = encDetents - 1; encSub = 0; }  // this panel's A/B are wired so +4 is counter-clockwise
  else if (encSub <= -4) { encDetents = encDetents + 1; encSub = 0; }
}

// ---------- UI state ----------
static lv_obj_t *arc;
static lv_obj_t *knobLabel;
static lv_obj_t *pressLabel;
static lv_obj_t *touchLabel;
static lv_obj_t *infoLabel;
static lv_obj_t *dot;

static int32_t lastDetents = 0;
static int arcValue = 50;
static int pressCount = 0;
static bool ledOn = false;
static const uint8_t BRIGHTNESS_STEPS[] = {100, 60, 30};
static uint8_t brightnessIdx = 0;

static uint8_t btnRaw = HIGH, btnStable = HIGH;
static uint32_t btnChangedAt = 0, btnDownAt = 0;
static bool longPressFired = false;
static bool wasTouched = false;
static uint32_t lastButtonPoll = 0;

static uint8_t *buf1 = nullptr;
static uint8_t *buf2 = nullptr;

// ---------- Hardware bring-up ----------
static void powerOn() {
  Wire.begin(I2C_SDA, I2C_SCL);
  pcf.pinMode(P0, OUTPUT);
  pcf.pinMode(P2, OUTPUT);
  pcf.pinMode(P3, OUTPUT);
  pcf.pinMode(P4, OUTPUT);
  pcf.pinMode(P5, INPUT_PULLUP);
  Serial.printf("[HW] PCF8574 %s\n", pcf.begin() ? "OK" : "NOT FOUND");

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

  ledcAttach(BACKLIGHT_PIN, 5000, 8);
  ledcWrite(BACKLIGHT_PIN, 255);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

static void displayOn() {
  if (!gfx->begin()) {
    Serial.println("[HW] display begin FAILED");
  }
  // Orientation and BGR bit, as in Elecrow's code.
  bus->beginWrite();
  bus->writeCommand(0x36);
  bus->write(0x08);
  bus->endWrite();

  // Color bars: red, green, blue, white, black. Confirms the panel and color order.
  const uint16_t colors[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_WHITE, RGB565_BLACK};
  const char *names[] = {"red", "green", "blue", "white", "black"};
  for (int i = 0; i < 5; i++) {
    gfx->fillScreen(colors[i]);
    Serial.printf("[TEST] screen %s\n", names[i]);
    delay(i < 4 ? 600 : 50);
  }

  bool ok = touch.begin(&Wire, TOUCH_ADDR);
  Serial.printf("[HW] touch controller %s\n", ok ? "OK" : "NOT FOUND");
}

static void lvglOn() {
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  const size_t bytes = sizeof(uint16_t) * SCREEN_W * 40;
  buf1 = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  buf2 = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!buf1 || !buf2) {
    Serial.println("[HW] LVGL buffer allocation in PSRAM FAILED");
  }

  lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, buf1, buf2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, [](lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
    lv_display_flush_ready(d);
  });
}

// ---------- UI ----------
static lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
  return l;
}

static void buildUi() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0E14), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  arc = lv_arc_create(scr);
  lv_obj_set_size(arc, 456, 456);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, arcValue);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x1E2633), LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);

  lv_obj_t *title = makeLabel(scr, &lv_font_montserrat_20, lv_color_hex(0x94A3B8), -120);
  lv_label_set_text(title, "Desk buddy hardware test");

  knobLabel = makeLabel(scr, &lv_font_montserrat_48, lv_color_hex(0xF8FAFC), -50);
  pressLabel = makeLabel(scr, &lv_font_montserrat_24, lv_color_hex(0xF8FAFC), 20);
  touchLabel = makeLabel(scr, &lv_font_montserrat_24, lv_color_hex(0xF8FAFC), 60);
  infoLabel = makeLabel(scr, &lv_font_montserrat_16, lv_color_hex(0x94A3B8), 120);
  lv_obj_set_style_text_align(infoLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(infoLabel, "Turn the knob  |  press: LED\nhold 1s: brightness  |  touch: dot");

  dot = lv_obj_create(scr);
  lv_obj_set_size(dot, 36, 36);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0xF59E0B), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

  lv_label_set_text_fmt(knobLabel, "%d", arcValue);
  lv_label_set_text(pressLabel, "Presses: 0  LED off");
  lv_label_set_text(touchLabel, "Touch: none");
}

static void setBrightness(uint8_t pct) {
  ledcWrite(BACKLIGHT_PIN, pct * 255 / 100);
  Serial.printf("[BACKLIGHT] %u%%\n", pct);
}

// ---------- Input handling ----------
static void handleKnob() {
  int32_t d = encDetents;
  if (d == lastDetents) return;
  int32_t delta = d - lastDetents;
  lastDetents = d;
  arcValue = constrain(arcValue + (int)delta * 2, 0, 100);
  lv_arc_set_value(arc, arcValue);
  lv_label_set_text_fmt(knobLabel, "%d", arcValue);
  Serial.printf("[KNOB] %s x%ld -> %d\n", delta > 0 ? "CW" : "CCW", (long)abs(delta), arcValue);
}

static void handleButton() {
  uint32_t now = millis();
  if (now - lastButtonPoll < 10) return;
  lastButtonPoll = now;

  uint8_t s = pcf.digitalRead(P5, true);  // active low
  if (s != btnRaw) {
    btnRaw = s;
    btnChangedAt = now;
  }
  if (btnRaw != btnStable && now - btnChangedAt >= 30) {
    btnStable = btnRaw;
    if (btnStable == LOW) {
      btnDownAt = now;
      longPressFired = false;
      Serial.println("[BUTTON] down");
    } else {
      Serial.println("[BUTTON] up");
      if (!longPressFired) {
        pressCount++;
        ledOn = !ledOn;
        digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
        lv_obj_set_style_arc_color(arc, lv_color_hex(ledOn ? 0x22C55E : 0x38BDF8), LV_PART_INDICATOR);
        lv_label_set_text_fmt(pressLabel, "Presses: %d  LED %s", pressCount, ledOn ? "on" : "off");
        Serial.printf("[BUTTON] press #%d, LED %s\n", pressCount, ledOn ? "on" : "off");
      }
    }
  }
  if (btnStable == LOW && !longPressFired && now - btnDownAt >= 1000) {
    longPressFired = true;
    brightnessIdx = (brightnessIdx + 1) % sizeof(BRIGHTNESS_STEPS);
    setBrightness(BRIGHTNESS_STEPS[brightnessIdx]);
    Serial.println("[BUTTON] long press");
  }
}

static void handleTouch() {
  bool t = touch.touched();
  if (t) {
    CST_TS_Point p = touch.getPoint(0);
    int x = constrain(p.x, 0, SCREEN_W - 1);
    int y = constrain(p.y, 0, SCREEN_H - 1);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(dot, x - 18, y - 18);
    lv_label_set_text_fmt(touchLabel, "Touch: %d, %d", x, y);
    if (!wasTouched) Serial.printf("[TOUCH] down %d,%d\n", x, y);
  } else if (wasTouched) {
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(touchLabel, "Touch: none");
    Serial.println("[TOUCH] up");
  }
  wasTouched = t;
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n[BOOT] desk buddy hw_test, Arduino core %s, LVGL %d.%d.%d\n",
                ESP_ARDUINO_VERSION_STR, lv_version_major(), lv_version_minor(), lv_version_patch());
  Serial.printf("[BOOT] PSRAM %u bytes, flash %u bytes\n", ESP.getPsramSize(), ESP.getFlashChipSize());

  powerOn();
  displayOn();
  lvglOn();
  buildUi();

  pinMode(ENC_A, INPUT);
  pinMode(ENC_B, INPUT);
  encState = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderIsr, CHANGE);

  btnRaw = btnStable = pcf.digitalRead(P5, true);
  Serial.println("[BOOT] ready");
}

void loop() {
  handleKnob();
  handleButton();
  handleTouch();
  lv_timer_handler();
  delay(5);
}
