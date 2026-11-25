# Architecture

## Two halves

- **`display.html`** is the whole UI in one browser file, with no dependencies. It's where screens are designed and tried; its comments name the matching firmware code and vice versa (`renderWeather` in display.html, `weather_panel.cpp` on the device). Its `localStorage` key `desk_buddy.v1` stands in for the device's flash, and `db.offset` shifts its clock for testing.
- **`firmware/desk_buddy/`** builds the same UI on the device with LVGL. Sizes are scaled up from the faux display for the real screen; colors come from its CSS (`theme.h`).

## Threads and cores

The ESP32-S3 has two cores. The rule of thumb: the UI and LVGL live on core 1, anything slow or periodic runs on core 0 and hands results over.

| Where | What |
|---|---|
| Core 1, Arduino `loop()` (16 KB stack) | Input, the panel framework, LVGL rendering, all LVGL calls |
| Core 0, panel task | Starts the RGB panel so its bounce-buffer refill interrupt runs on core 0 (keeps LVGL's core free) |
| Core 0, flush task | Copies LVGL's 48-line strips into the PSRAM framebuffer and writes back the cache, overlapping the next strip |
| Core 0, painter (`painter.h`) | Full-screen backgrounds pixel by pixel: Home's faces, Weather's glow |
| Core 0, weather task | Geocoding and forecasts from Open-Meteo (`weather.cpp`) |
| Core 0, thumbs task | Home's customize-sheet previews, only while the sheet is open |
| Core 0, buzzer task | Plays step sounds with 1 ms ticks |
| Core 0, boot prep | Pre-renders Breathe's orb and the coin at boot |

Core 0's idle task is watched by the task watchdog (5 s): every loop there must yield (`vTaskDelay(1)`). Core-0 code never calls `store::` or `tz::`; shared data (weather, search results) is behind a mutex.

## Screens

`app.h` has the framework:
- **`Screen`**: `build(root)` creates widgets from the screen's own state (the app may destroy and rebuild it at any time), plus `rotate`, `press`, `tick`, `swipe`, `swipeVertical` and flags (`isEntry`, `isAlert`, `isGame`, `active`, `ownsTouch`).
- **`Panel`**: a top-level screen in the swipe carousel (`panels.cpp` has the default order: Home, Weather, World, Focus, Timers, Breathe, Habits, Days, Play, Settings). `visible()` lets Weather hide itself without Wi-Fi. `background()` runs every loop for panels not on screen (pre-painting).
- **Modes** (`modes.h`): reusable sub-screens pushed on top: `DetailMode`, `PickerMode`, `FieldsMode`, `TextMode` (the ring keyboard), alerts.
- `ListWheel` (`list_wheel.h`): every scrolling list (knob, drag, flick, tap).
- `TimerFace` / `Dial`: the edge dial shared by Focus, timers and Brightness, with touch on the ring.
- Screen changes are applied from `loop()`, never inside an LVGL event callback.

Input: the knob (`board.cpp` decodes it in an interrupt), knob press/hold, touch (LVGL gestures plus direct polling where needed). A 3 s hold of the knob or a still finger goes Home from anywhere; idle return to Home after a set time (Settings).

## Drawing big things

- **`BgCanvas` (`painter.h`):** two 480x480 RGB565 buffers in PSRAM per panel. The last finished background shows at once when the panel is rebuilt; a new one is painted on the core-0 painter, then swapped in. Home asks for a new background once a minute.
- **Home faces** (`home.cpp`): the `FACES` table, `paintScene()` (the standard painter: background, ring, glow; 8.8 fixed point, glow lookup table, ordered dithering), and custom painters such as `paintHeat`. See `docs/adding-a-face.md`.
- **Dithering** (`dither.h`): gradients go through an 8x8 Bayer matrix so they don't band on the 565 panel.
- **Fast travel tray** (`tray.cpp`, pull down from the top): draws straight into the framebuffer, because a full-screen slide through LVGL runs ~8 fps. LVGL invalidation is paused while it's open, a touch it claims is hidden from LVGL, and anything built under it must be laid out (`settleLayout`) before LVGL redraws uncovered rows.
- **Moon** (`moon.cpp`): real phase from the date, maria, flipped south of the equator. A 72 px ARGB sprite on Home, painted per pixel into previews.

## Data and settings

All persistent state is in NVS through `store.h` (the faux display's `localStorage`). Keys:

| Area | Keys |
|---|---|
| Home | `homeface` (index into `FACES`), `homeshow` (bitmask: 1 weather, 2 habits, 4 countdown, 8 timers) |
| Records | `habits`, `cdowns` (records split by `\x1e`, fields by `\x1f`), `cities` (World preset indexes) |
| Time | `tzcity`, `tzloc`, `tzoff`, `tzname`, `tzlat`, `tzlon`, `clock` (12/24 h), `autotime` |
| Weather | `loc` (`@lat,lon|Name|Region, Country|CC` once picked), `wxunits`, `wxcc` |
| Wi-Fi | `ssid`, `wifipw` (stored on the device only) |
| Reminders | `remall`, `remfrom`, `remto`, `remdays`, per-reminder intervals |
| Brightness | `ad_on`, `ad_wake`, `ad_bed`, `ad_max`, `ad_min`, plus the manual level |
| Sound | `sndvol` (0 off to 3 high), `sndclick`, `sndpick` (`event=sound,...` with the lab's ids, changed events only) |
| Other | `idlehome`, `focusMin`, `focusDay`, `focusN`, `breath`, `diceset`, `coinh`, `coint`, `bestpong`, `bestreflex`, panel order |

Values used as indexes are clamped on read. `store::setIntLater` saves after 1.5 s of no change, for values a knob spins through.

## Network (optional)

- **Setup:** Settings > Wi-Fi starts an open access point `DeskBuddy-XXXX` with a captive setup page on the phone (network, password, or a manual date and time; weather location; time zone). `net.cpp`.
- **Time:** SNTP once connected. Until a time source is trusted (`clockx::trusted`: NTP, set by hand, or a forecast's Date header), Home says "Waiting for network time" or "Clock not set" instead of a wrong date. The board has no battery-backed clock.
- **Weather:** Open-Meteo, free, no key or account. Over **plain HTTP** (HTTPS is compiled out of this Arduino core build), so the location and forecast travel unencrypted. A bounded body reader (15 s deadline, 64 KB cap, yields) replaced `getString()`.
- **Time zones** (`tz.cpp`): built-in POSIX rules for preset cities, evaluated by our own code; locations without a preset follow the forecast's UTC offset.

## Generators

Assets are generated from scripts in `firmware/desk_buddy/tools/`, mostly from the faux display's SVGs so the two match:

| Script | Writes |
|---|---|
| `gen_icons.py` | `src/icons.c` (panel icons) |
| `gen_wx_icons.py` | `src/wx_icons.c` (weather icons) |
| `gen_remind_icons.py` | `src/remind_icons.c` |
| `gen_sprites.py` | `src/sprites.c` (sun, fallback moon) |
| `gen_tray.py` | `src/tray_assets.c` |
| `gen_font.py` | `src/font_*.c` (large digit fonts from a TTF in `tools/fonts/`) |
| `gen_heat.py` | `src/heat_assets.cpp/.h` and `tools/heat_assets.js` (Heat face art) |
| `gen_sounds.js` (Node) | `src/sound_data.h` (the sound library, and each event's default and suggestions, from `sound-lab.html`) and the faux display's `SOUND_CHOICES` |
