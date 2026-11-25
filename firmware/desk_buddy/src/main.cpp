// Desk buddy firmware for the CrowPanel 2.1" rotary display.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "app.h"
#include "autodim.h"
#include "board.h"
#include "clock.h"
#include "net.h"
#include "panels.h"
#include "reminders.h"
#include "store.h"
#include "records.h"
#include "timers.h"
#include "tray.h"
#include "weather.h"
#include "sounds.h"

void renderOrb();   // breathe.cpp
void renderCoin();  // play.cpp

// The main loop runs LVGL's rendering; with the default 8 KB it was down to ~1 KB spare before
// Home's customize sheet (many small objects) overflowed it. 16 KB leaves room.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
  Serial.setRxBufferSize(8192);  // sound lab Z commands arrive as one long line
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] desk buddy, Arduino core %s, LVGL %d.%d.%d\n", ESP_ARDUINO_VERSION_STR,
                lv_version_major(), lv_version_minor(), lv_version_patch());

  store::begin();
  habits::begin();
  countdowns::begin();
  clockx::begin();
  net::begin();
  weather::begin();
  board::begin();
  sounds::begin();
  app::begin(makePanels());
  tray::begin();  // fast travel tray: sheet background painted on core 0
  // Breathe's orb and the coin, drawn once in the background so opening them is instant.
  xTaskCreatePinnedToCore([](void *) {
    renderOrb();
    renderCoin();
    vTaskDelete(nullptr);
  }, "prep", 4096, nullptr, 1, nullptr, 0);
  reminders::begin();

  // Draw the first frame before turning the backlight on, so there's no flash of garbage.
  lv_timer_handler();
  autodim::begin();
  Serial.printf("[boot] internal RAM free %u KB (largest block %u KB)\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  sounds::play(sounds::STARTUP);
  Serial.printf("[boot] ready, boot #%lu, brightness %.0f%% (auto %s)\n", (unsigned long)store::bootCount(),
                autodim::current(), autodim::enabled() ? "on" : "off");
}

void loop() {
  clockx::poll();
  net::tick();
  weather::tick();
  app::loop();
  reminders::tick();
  timers::tick();
  autodim::tick();
  store::tick();
  uint32_t idle = lv_timer_handler();
  if (tray::live()) return;  // the tray draws every pass; sleeping would cap its frame rate
  delay(idle < 5 ? idle : 5);
}
