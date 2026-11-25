#include "painter.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {

constexpr int W = 480;

struct Job {
  BgCanvas::PaintFn *fn;
  uint16_t *buf;
  volatile bool *done;
};
QueueHandle_t jobs = nullptr;
bool workerOk = false;  // without the core-0 painter, paints run on the UI thread (slower, still right)

void worker(void *) {
  Job j;
  for (;;) {
    if (xQueueReceive(jobs, &j, portMAX_DELAY) != pdTRUE) continue;
    (*j.fn)(j.buf);
    delete j.fn;
    *j.done = true;
  }
}

}  // namespace

void BgCanvas::ensure() {
  if (!buf[0] && !noBuffers) {
    for (auto &b : buf) b = (uint16_t *)heap_caps_malloc(W * W * 2, MALLOC_CAP_SPIRAM);
    if (!buf[0] || !buf[1]) {
      Serial.printf("[painter] no room for backgrounds (PSRAM largest block %u KB)\n",
                    (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
      for (auto &b : buf) { heap_caps_free(b); b = nullptr; }
      noBuffers = true;
    }
  }
  static bool tried = false;
  if (!tried) {
    tried = true;
    jobs = xQueueCreate(4, sizeof(Job));
    workerOk = jobs && xTaskCreatePinnedToCore(worker, "painter", 6144, nullptr, 1, nullptr, 0) == pdPASS;
    if (!workerOk) Serial.println("[painter] no painter task: painting on the UI thread");
  }
}

lv_obj_t *BgCanvas::build(lv_obj_t *root) {
  ensure();
  canvas = lv_canvas_create(root);
  lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  // The panel's widgets are rebuilt often; forget the canvas when LVGL deletes it.
  lv_obj_add_event_cb(canvas, [](lv_event_t *e) {
    auto *self = (BgCanvas *)lv_event_get_user_data(e);
    if (self->canvas == lv_event_get_target(e)) self->canvas = nullptr;
  }, LV_EVENT_DELETE, this);
  collect();
  if (front >= 0) show();
  else lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
  return canvas;
}

void BgCanvas::show() {
  if (!canvas || noBuffers || front < 0) return;
  lv_canvas_set_buffer(canvas, buf[front], W, W, LV_COLOR_FORMAT_RGB565);
  lv_obj_remove_flag(canvas, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(canvas);
}

void BgCanvas::request(int64_t key, PaintFn paint) {
  ensure();  // a panel may paint ahead, before it's ever been shown
  if (noBuffers) return;
  if (key == frontKey || (running && key == runKey)) {
    if (!running || key == runKey) wantKey = INT64_MIN, want = nullptr;
    return;
  }
  wantKey = key;
  want = std::move(paint);
  if (syncFirst && front < 0 && !running) {
    // Nothing to show yet: paint here, once.
    uint32_t t0 = millis();
    want(buf[0]);
    front = 0;
    frontKey = key;
    wantKey = INT64_MIN;
    want = nullptr;
    show();
    Serial.printf("[painter] first paint %lu ms\n", (unsigned long)(millis() - t0));
    return;
  }
  start();
}

void BgCanvas::start() {
  if (running || !want) return;
  running = true;
  done = false;
  runKey = wantKey;
  Job j{new PaintFn(std::move(want)), buf[front < 0 ? 0 : 1 - front], &done};
  want = nullptr;
  wantKey = INT64_MIN;
  if (!workerOk) {  // paint here; collect() swaps it in
    (*j.fn)(j.buf);
    delete j.fn;
    done = true;
    return;
  }
  xQueueSend(jobs, &j, portMAX_DELAY);
}

void BgCanvas::collect() {
  if (running && done) {
    running = false;
    front = front < 0 ? 0 : 1 - front;
    frontKey = runKey;
    show();
  }
}

void BgCanvas::tick() {
  collect();
  if (!running) start();
}
