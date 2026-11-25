# Desk buddy: notes for coding agents

Read this before changing anything. It lists what the project is, how to build and check it, and the rules that came from real crashes and regressions. Details live in `docs/`.

## What this is

Firmware for the Elecrow CrowPanel 2.1" rotary display (ESP32-S3, 8 MB PSRAM, 16 MB flash, 480x480 round touch screen, knob with press), plus a browser version of the same UI.

- `firmware/desk_buddy/`: the firmware (PlatformIO, Arduino core 3.3.8, LVGL 9.6.0). `src/` is the code, `tools/` the generators and test scripts.
- `display.html`: the **faux display**, a single-file browser version of the device. It's the design spec: new screens are designed here first, then built in firmware, and the two are kept in sync.
- `firmware/hw_test/`: a minimal hardware test (screen, touch, knob, backlight, LED).
- `sound-lab.html`: designs buzzer sounds and exports a `sounds.h` (the buzzer add-on is optional).
- `stand/`: printable desk stand (STLs and print notes).
- `mockups/`: design mock-ups that decisions point back to ("option B in reminder-mockups.html").
- `archive/`: removed features with restore notes.
- `site/`: the GitHub Pages site (web installer with ESP Web Tools, getting started guide). `site/build.sh` assembles `_site/` from it plus `display.html`, `sound-lab.html` and the built firmware; `.github/workflows/site.yml` builds and publishes on every push to `main`.
- `docs/`: `hardware.md`, `architecture.md`, `lessons.md`, `testing.md`, `ux.md`, `adding-a-face.md`.

The device has no accounts and no cloud. Wi-Fi is optional (network time and weather from Open-Meteo); everything else works offline.

## Build, flash, check

```sh
cd firmware/desk_buddy
~/.platformio/penv/bin/pio run                 # build
~/.platformio/penv/bin/pio run -t upload       # flash (port in platformio.ini)
~/.platformio/penv/bin/python tools/screenshot.py out.png        # screenshot over USB serial
~/.platformio/penv/bin/python tools/serial_log.py 5             # log serial for 5 min
```

Faux display: `python3 -m http.server 8767 --bind 127.0.0.1` in the repo root, then open `http://127.0.0.1:8767/display.html`.

The platform is pinned in `platformio.ini` (a Tasmota/pioarduino build with Arduino core 3.3.8); the plain `espressif32` platform ships the older 2.x core and won't build this. Screenshots in `site/img/` come from the real device (`tools/screenshot.py`); keep personal data (Wi-Fi names, home city) out of them.

Serial test commands (knob `K<n>`, press `P`, hold `H`, swipe `W1`/`W-1`, swipe up `U`, memory `M`, clock `T<epoch>`, ...) are listed in `docs/testing.md`. Touch can't be injected over serial: say so when a change needs a real finger to verify.

## Rules (each one cost a crash, a hang or a visible glitch)

Threads and timing
- **Core 0's idle task is watched (5 s, then a reboot).** Any long loop on core 0 (painters, boot prep, the weather task) must `vTaskDelay(1)` every few rows or reads. Never busy-wait there: `HTTPClient::getString()` did, and a stalled server reboot-looped the device.
- **LVGL only on the UI core** (the Arduino loop). Background tasks fill buffers; the UI thread hands them to LVGL.
- **No `store::` (NVS) or `tz::` calls from core 0.** Pass values in instead.
- Screen changes happen from `loop()`, never inside an LVGL event callback (`app.cpp`).

Rendering
- **Never `lv_label_set_text` with unchanged text per frame**, and don't restyle widgets that didn't change: each one redraws.
- **Never `lv_obj_move_foreground` per input:** it invalidates the whole parent (54 ms per knob click).
- **Never animate `opa` on objects;** use `text_opa`/`bg_opa`/`border_opa` (`opa` renders into an off-screen layer).
- A full-screen redraw costs ~40-55 ms; refresh is capped at ~30 fps. Redraw only what changed.
- Anything drawn outside its parent (a selected chip's ring and glow) needs the parent to make room (`ui::roomForGlow`), or LVGL's strip renderer clips it.
- Full-screen pixel work (Home's background, Weather's glow) goes through `BgCanvas` (`painter.h`), painted on core 0, never on the UI thread.
- Dithered images on the page background must use LVGL's own 565 page color, or a square edge shows.
- `lv_display_enable_invalidation` is a counter: calls must be balanced pairs.

Memory
- Internal RAM is tight (~80-100 KB free). Big buffers go in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`), and every allocation needs a fallback (draw flat, skip the preview, run without the task). Check with serial `M`.
- The main loop's stack is 16 KB (`SET_LOOP_TASK_STACK_SIZE` in `main.cpp`); 8 KB overflowed.

Text
- The built-in Montserrat fonts have only ASCII, the degree sign and U+2022 (bullet). No other non-ASCII in UI text: "..." not "…", fold accents (`weather::ascii`).
- Big digit fonts are generated (`tools/gen_font.py`); keep the clock's 148 px line height.

Data
- Settings and records live in NVS via `store.h`. Values used as indexes are clamped on read. Add new list entries (time zone presets, Home faces) at the **end**: saved indexes must keep meaning the same thing.

## Workflow

- Design in `display.html` first, then build the firmware to match, then check both. When you change one, update the other in the same change.
- Generated files (`src/*icons*.c`, `src/sprites.c`, `src/tray_assets.c`, `src/heat_assets.*`, `src/font_*.c`, `src/sound_data.h`) come from `tools/gen_*` scripts. Edit the generator or its source (`sound-lab.html` for sounds), not the output.
- Interaction rules are in `docs/ux.md`: rotate changes the selection or value, press confirms, swipe changes panels, tap acts on what you touched, no undo for non-destructive changes, knob acceleration where values move far, every list and dial also takes touch.
- Before a refactor that shouldn't change pixels, capture before/after screenshots (and, for painters, hashes of the painted buffer) and compare. `tools/sheetwalk.sh` walks Home's customize sheet.
- Scripted serial tests press real settings: check the device's state before and after (a test once hid a panel and reordered the list).
- Screenshots occasionally come back with the lower part in noisy colors: a byte dropped in the USB transfer, often when a log line printed mid-transfer. Retake it.

## Where to look

- Adding a Home face: `docs/adding-a-face.md`.
- Pins, power, flashing trouble, the optional buzzer: `docs/hardware.md`.
- Tasks, panels, modes, the painter, the fast travel tray, NVS keys: `docs/architecture.md`.
- Past bugs and their causes: `docs/lessons.md`.
