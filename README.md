# Desk buddy

A small round desk gadget built on the Elecrow CrowPanel 2.1" rotary display: a clock with a living sky, focus timer, timers, breathing guide, habits, countdowns, world clocks, weather, break reminders and a few knob games. Everything is driven by one knob (turn and press) and the round touch screen.

It has no accounts, no cloud and no phone app. Wi-Fi is optional and only used for network time and the weather; everything else works offline. Nothing on it talks to your work tools.

## Panels

| Panel | What it does |
|---|---|
| Home | Clock, date, weather line and glance stats over a choice of faces (Sun ring, Living sky, Plain, Heat). Swipe up from the bottom edge to pick a face and what shows. |
| Weather | Now, hourly and 7-day from Open-Meteo (with Wi-Fi). Turn to scrub, press to switch. |
| World | Cities with a 24-hour day/night ring. |
| Focus | A focus timer on an edge dial. |
| Timers | Labeled timers and a stopwatch. |
| Breathe | Paced breathing patterns. |
| Habits | Daily check-offs with streaks. |
| Days | Countdowns to dates. |
| Play | Ring pong, dice, coin, reflex test. |
| Settings | Wi-Fi, weather, time zone and format, brightness and auto brightness, sound, break reminders (stand, water, eyes), panels (hide, show, reorder), return to Home. |

Pull down from the top edge for the fast travel tray (every panel in one row, and a brightness button).

## What you need

- **Elecrow CrowPanel 2.1" HMI ESP32 Rotary Display** (ESP32-S3, 480x480 round IPS, touch, knob). It comes with a USB-C cable.
- A USB-C power supply or a computer to power it.
- Optional: a 3D-printed desk stand (`stand/`: STLs and assembly notes).
- Optional: a KY-006 passive buzzer module for sounds: soft knob clicks, timer and reminder chimes, game sounds and a boot melody (see `docs/hardware.md`). The stand has rails for it.

## Install it

**Easiest:** open **https://freakintoddles2.github.io/desk-buddy/** in Chrome or Edge, plug the board in, and click Install. The [getting started guide](https://freakintoddles2.github.io/desk-buddy/guide.html) walks through it, with screenshots.

**From source:** you need [PlatformIO](https://platformio.org/install/cli) (the CLI, or the VS Code extension).

```sh
git clone <this repo>
cd <repo>/firmware/desk_buddy
pio run -t upload
```

- First flash: hold **BOOT** on the board while plugging it in (the factory demo doesn't open the USB port by itself). After that, uploads reset the board on their own.
- PlatformIO finds the board's port by itself; `platformio.ini` shows how to set one if you have several serial devices.
- Trouble? See `docs/hardware.md`.

## Set it up

1. Turn the knob to move, press to choose, swipe to change panels, hold to go back.
2. For network time and weather: Settings > Wi-Fi starts setup mode. Join the `DeskBuddy-XXXX` network on your phone; a setup page opens where you pick your Wi-Fi, enter the password and type your city. Without Wi-Fi, set the date and time by hand in Settings > Date & time.

## Try it in a browser

Online: **https://freakintoddles2.github.io/desk-buddy/display.html**. Or locally:

`display.html` is the whole UI in one file, used to design and test screens before building them.

```sh
python3 -m http.server 8767 --bind 127.0.0.1
# open http://127.0.0.1:8767/display.html
```

Arrows turn and swipe, Enter presses, T opens the fast travel tray, U the Home customize sheet.

## Sound lab

**https://freakintoddles2.github.io/desk-buddy/sound-lab.html** (or open `sound-lab.html` locally)

Pick the sound for each thing the desk buddy does (knob clicks, timers, reminders, games, the boot melody) from suggestions or the whole library of about 160 sounds, or make your own on a piano-roll editor. With the board plugged in by USB (Chrome or Edge), click **Connect** to hear sounds on the real buzzer, **Get from desk buddy** to load what it plays now, and **Send to desk buddy** to put your choices on it. The same choices are on the device in Settings > Sound > Sounds. Needs the optional KY-006 buzzer to hear anything on the device.

## Network and privacy

- The Wi-Fi password and your settings are stored only on the device's flash.
- Weather comes from [Open-Meteo](https://open-meteo.com/) (free, no key, no account). This Arduino core build has HTTPS compiled out, so the weather requests, including your location, go over **plain HTTP**.
- The setup network is open while setup mode is on (it turns off when you finish).
- Network time uses public NTP servers.

## Make it yours

- **New Home face:** `docs/adding-a-face.md` (Heat is the worked example: its own painter, palms and skyline art, a slanted clock font).
- **New panel or screen:** design it in `display.html`, then build it in `firmware/desk_buddy/src` following the existing panels and `docs/ux.md`.
- **Using an AI coding agent?** Point it at `AGENTS.md` (Claude Code reads it through `CLAUDE.md`). It lists the build commands and the rules that came from real crashes and glitches, so it doesn't repeat them.

Ideas that are deliberately not built in, if you want to add them yourself:
- **Bluetooth (BLE).** Left out on purpose: part of the appeal is a device that doesn't repeat your phone's notifications. The ESP32-S3 has BLE 5 only (no classic Bluetooth audio). With an iPhone and no app, Apple's Notification Center Service, Media Service (the knob as a volume knob) and Current Time Service would work; Android needs a companion app. It could also act as a BLE media/volume knob for a computer (HID consumer controls). Measure memory first: the BLE stack needs tens of KB of internal RAM and only ~80-100 KB is free.
- **Home Assistant.** Removed: home networks usually aren't reachable from a desk without HTTPS, and a token would sit unencrypted in flash.
- **Over-the-air updates.**

## Status

Working and in daily use: all panels above, Wi-Fi setup, weather, time zones, auto brightness, reminders, Home faces and the customize sheet, the fast travel tray.

Not done yet:
- The faux display lags the firmware in a few spots (the time zone "location's time" row, "Waiting for network time" on Home, the "Getting the weather for..." step).
- Not every touch gesture has been checked with a real finger on every screen.

## Repository layout

| Path | Contents |
|---|---|
| `firmware/desk_buddy/` | The firmware (PlatformIO). `src/` code, `tools/` generators and test scripts, `include/lv_conf.h` |
| `firmware/hw_test/` | A minimal hardware test (screen, touch, knob, backlight, LED) |
| `display.html` | The browser version of the UI |
| `sound-lab.html` | Picks the sound for each event, designs new ones, and reads and sends them to the device over USB (Web Serial) |
| `site/` | The GitHub Pages site: web installer, getting started guide (`site/build.sh` assembles it; `.github/workflows/site.yml` publishes it) |
| `mockups/` | Design mock-ups behind the current screens |
| `stand/` | Printable desk stand (STLs and assembly notes) |
| `archive/` | Removed features with restore notes (Spin the wheel) |
| `docs/` | Hardware, architecture, testing, interaction rules, lessons learned, adding a face, reference links |

## Credits and licenses

- [LVGL](https://lvgl.io/) (MIT), [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX), [Adafruit CST8XX](https://github.com/adafruit/Adafruit_CST8XX), Adafruit BusIO, [PCF8574 library](https://github.com/xreef/PCF8574_library), [ArduinoJson](https://arduinojson.org/): fetched by PlatformIO, under their own licenses.
- Fonts: Montserrat and Anton, SIL Open Font License 1.1 (`firmware/desk_buddy/tools/fonts/`). LVGL's built-in fonts are Montserrat.
- Weather data by [Open-Meteo.com](https://open-meteo.com/), licensed CC BY 4.0.
- Board pins and power-up sequence from Elecrow's examples.
- The Heat face is based on a design concept by a coworker.

This project's own code, docs and stand files: MIT (`LICENSE`). The fonts keep their own OFL license.
