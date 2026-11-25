# Lessons: problems already hit and solved

Symptom, cause, fix. Read this before chasing a crash, a stutter or a glitch: it may be here.

## Crashes and hangs

- **Reboot loop when the weather server stalls.** `HTTPClient::getString()` busy-waits in `Stream::timedRead` without yielding; on core 0 that starves the idle task and the 5 s watchdog resets the board, and the retry repeats it. Fix: own body reader (`http.useHTTP10(true)`, read while `available()`, `vTaskDelay(1)` when empty, 15 s deadline, 64 KB cap).
- **About 1 in 3 boots crashed in Home's repaint** after more boot-time pre-rendering was added on core 0. Long core-0 loops without yields pushed the idle task past 5 s. Fix: `vTaskDelay(1)` every few rows in every core-0 loop.
- **"Unhandled debug exception" with a corrupted backtrace** when opening Home's customize sheet: the main loop's 8 KB stack overflowed (LVGL rendering runs there). Fix: 16 KB (`SET_LOOP_TASK_STACK_SIZE` in `main.cpp`). Peak use is ~8 KB.
- **Crash when two full-screen buffers were added for the tray:** two 460 KB buffers starved the Focus dial and other PSRAM users. Fix: no full-screen buffers in the tray; a 64-level pre-dithered palette and small row buffers.
- **Screen froze after "restoring" LVGL invalidation:** `lv_display_enable_invalidation` is a counter, not a flag. Fix: always balanced enable/disable pairs.
- **Unchecked allocations:** every PSRAM buffer and task creation now has a fallback (no background, flat drawing, no previews, run on core 1). Keep it that way for new ones.

## Stutter and lag

- **Settings list scrolled at 8 fps.** In order of gain: LVGL draw buffers in internal SRAM (2 x 48 lines) instead of PSRAM; fading rows with `text_opa`/`bg_opa` instead of `opa` (which renders each row off-screen); `-O2` instead of `-Os`; starting the panel from a core-0 task (its refill interrupt moves off LVGL's core); flushing on a core-0 task. ~120 ms per frame became ~32 ms (the 30 fps cap).
- **Swipes into Home and Weather lagged:** both repainted their backgrounds (1.4-1.9 s) on the UI thread on every visit. Fix: `BgCanvas` (keep the last background, paint new ones on core 0, swap).
- **Idle Focus redrew 32 times a second:** a label was set to the same text every frame. Rule: never `lv_label_set_text` per frame with unchanged text.
- **Each knob click on the ring keyboard cost 54 ms:** restyling a key plus `lv_obj_move_foreground` redrew the whole screen. Fix: one highlight object moved over the ring (~3 ms). Rule: don't reorder objects per input.
- **Focus swipe-in stutter:** the edge dial was drawn with LVGL arcs in every 48-row strip. Fix: draw the dial once into a canvas, redraw only the slice that changed.
- **First visit to Breathe stalled 0.5-2 s:** the orb was rendered on demand. Fix: pre-render at boot on core 0.
- **Home's background took ~2 s after picking a face:** float math per pixel. Fix: 8.8 fixed-point colors, a glow lookup table on squared distance, integer dithering, floats only where needed (3-4x faster).
- **Settings re-styled every row once a second while idle.** Fix: `ListWheel::setItems` skips identical items.

## Visual glitches

- **Red and blue swapped:** Elecrow's MADCTL 0x08 (BGR) is wrong for these panels. Fix: 0x00.
- **A selected chip's bottom ring was clipped:** LVGL draws in strips and skips a container that is outside the strip, even when a child's glow reaches into it. Fix: the parent declares room (`ui::roomForGlow`).
- **A square around a dithered image on the page:** the image's background didn't match LVGL's truncated 565 fill of the page color. Fix: quantize from LVGL's own 565 value.
- **Boxes instead of characters:** the built-in Montserrat only has ASCII, the degree sign and U+2022. Fix: "..." instead of "…", ASCII-folded place names.
- **LVGL radial gradient with % coordinates rendered garbage.** Use pixel coordinates.
- **Brightness "flashed in twice" when opened from the tray:** a screen built under the tray wasn't laid out yet; LVGL laid it out on the tray's first partial refresh while invalidation was on, redrawing every object over the tray. Fix: `settleLayout()` right after building.
- **A preview silently went missing:** the Focus dial's 460 KB cache left too little PSRAM. Fix: the customize sheet releases that cache when it opens.
- **The accent selection bar vanished on Living sky's blue:** color-on-sky contrast. Fix: white bar on light faces, and stat colors swapped for white or black under 3:1 (`readable()`).

## Input

- **Two knob clicks per item:** the encoder is half-step (rests at 00 and 11). Fix: count a step at each resting state.
- **Taps landed low:** `TOUCH_Y_SHIFT` moves touches up 10 px.
- **Tapping a tray icon sometimes jumped one panel:** the touch controller reports (0, 0) on lift-off; direct polling caught it as a 250 px drag. Fix: treat raw (0, 0) as no touch.
- **List flicks stopped dead:** a vertical-gesture handler called `lv_indev_wait_release` on every flick, so lists got PRESS_LOST instead of RELEASED. Fix: only a screen that uses the vertical swipe ends the touch; lists treat PRESS_LOST as a release.
- **A drag's release also tapped a row:** LVGL sends SHORT_CLICKED after RELEASED. Fix: 8 px slop, and a touch that stops a moving list isn't a tap.

## Network and time

- **"14 days old" weather after boot:** the forecast arrived before network time and was stamped with the build-time clock. Fix: stamp it from the response's `current.time`.
- **Wrong clock for places without a time-zone preset** (Bogota showed New York's DST). Fix: fall back to the forecast's UTC offset ("location's time").
- **Clock silently on build time when NTP is blocked.** Fix: a trusted-clock flag; Home says so instead of showing a date; the forecast's Date header counts as a time source.
- **Null forecast fields showed as 0°, "Clear".** Fix: require current temperature and code; end lists at the first null; leave out missing humidity, wind or rain chance.

## Testing gotchas

- Screenshots with the lower part in noisy colors: a byte dropped in the USB transfer, often when a log line printed during it. Not a display bug; retake.
- With the panel interrupt on core 0, USB serial is slower: a screenshot takes several seconds. `-DPANEL_CORE=1` makes screenshots fast again (frames get slower).
- Scripted serial tests press real settings. One hid Weather and reordered the panels. Check state before and after.
- Touch can't be injected over serial: changes to touch handling need a real finger.
- When a refactor shouldn't change what's drawn, hash the painted buffers before and after; a mismatch is a bug even if it looks the same.
