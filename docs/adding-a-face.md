# Adding a Home face

A Home face is the background behind Home's clock: Sun ring, Living sky, Plain and Heat ship today. Each face is one entry in a table, kept in two places that must match:

- Firmware: `FACES` in `firmware/desk_buddy/src/home.cpp`
- Faux display: `FACES` in `display.html`

The customize sheet (swipe up from the bottom of Home), the knob order, the dots and the previews all come from the table. You never edit them.

## Rules first

- **Append only.** The device saves the chosen face as its index in `FACES` (NVS key `homeface`). Add new faces at the end, and never reorder or delete one, or everyone's saved face silently changes. The faux display saves the `key` instead, but keep the same order there too.
- **Paint budget under ~1 s.** The background is painted pixel by pixel on core 0 once a minute and when the sheet shows previews. Current times at 480 px: Plain ~20 ms, Sun ring ~440 ms, Heat ~585 ms, Living sky ~0.3 s clear up to ~2 s in a storm (its weather layers; the last painted sky stays up while the next one paints). About 2 s feels laggy when switching to a face.
- **Keep the 148 px clock line height.** A different clock font must keep Home's layout (see "A custom clock font" below).
- **Check the text.** Clock, date, weather line and stats sit over the middle of the screen. Pick text colors per face (day and night) and look at it at several times of day.

## Level 1: a variation (fields only)

Most looks are a few fields on the standard painter. Example: a dark face with the ring and a warm glow.

```cpp
// home.cpp, at the end of FACES
{.name = "Ember", .ring = true, .moonHalo = true, .glowReach = 0.5f, .glowDay = 0.35f, .glowTint = 0xFF8A3D},
```

```js
// display.html, at the end of FACES
{ key: 'ember', name: 'Ember', ring: true, moonHalo: true, glowReach: .5, glowDay: .35, glowTint: '#ff8a3d' },
```

Fields (the defaults are those of Sun ring, without its ring):

| Field | What it does |
|---|---|
| `name` | Logs only; the sheet shows no names. |
| `body` | `false` gives a flat dark screen that's never repainted (Plain). |
| `bg` | `DARK` (the screen color) or `SKY` (the sky's top-to-horizon gradient for the time of day). |
| `ring` | The 24-hour sky ring. |
| `path` | Where the sun/moon goes: `RING` (on the ring), `ARC` (an arc over the upper half), `SINK` (the sun sinks in the middle through the day, the moon takes the arc). |
| `sunR`, `moonR` | Body radius in px (480 px screen). The moon sprite on Home uses `moonR`; the sun sprite has a fixed size and `sunR` only sizes the preview's sun. |
| `moonHalo` | Soft halo around the moon sprite (looks right on dark faces). |
| `glowReach`, `glowDay`, `glowNight`, `glowNightLit` | Glow radius (fraction of the distance to the farthest corner) and strength by day, and at night (base + this times the moon's lit fraction). Falloff is quadratic. |
| `glowTint` | Day glow color; `0` uses the horizon color at that hour. |
| `day`, `night` | Text colors (`Ink`): `clock`, `date`, `wx` (whole weather line), `dim` (stat labels, and the weather line if `wx` is 0), `shadow` (a drop shadow under the clock). `0` keeps the usual color. If day and night differ, Home rebuilds at sunrise and sunset. |
| `lightBg` | A bright background: the selection bar turns white and stat colors are swapped for white or black when they'd be under 3:1 contrast. The contrast check samples the standard `bg` gradient, so it's only accurate for faces without their own painter. |
| `paint` | Own painter instead of the standard one (level 2). |
| `daySprite` | `false` when the painter draws the sun itself (Home then shows no sun sprite by day). |
| `weather` | The sky follows the weather (`skywx.cpp`, `paintWeatherSky` in `display.html`): clouds, rain, snow, fog and storms per WMO code. Set on Living sky, with `paint = paintWeatherSky`. When clouds cover the sun or moon, the painter draws it dimmed and Home hides the sprite. |
| `clockFont` | A generated clock font; `nullptr` keeps `font_clock_120`. Faux display: a CSS class `font-<name>`. |
| `upper` | Date in spaced capitals. (Not the weather line: in capitals it no longer fits.) |

## Level 2: your own painter

When the look needs shapes (palms, a skyline, rays), write a painter. Heat (`paintHeat` in `home.cpp`, `heatNodes` in `display.html`) is the worked example.

Firmware painter contract (`PaintFn`):

```cpp
bool paintMyFace(uint16_t *buf, int n, const Scene &s, const std::function<bool()> &stop);
```

- Fill the `n` x `n` RGB565 buffer. `n` is 480 for Home and 240 or 138 for the sheet's previews. Work in 480 px coordinates: `X = (x + 0.5f) * 480.0f / n`.
- In previews (`n != 480`), fill the corners outside the circle with `SCREEN_HEX` and paint the moon in yourself at night (copy the `moon::pixel` block from `paintHeat`). On Home the sun and moon are sprites drawn on top.
- `s.night`, `s.hour`, `s.bx`/`s.by` (the sun or moon from `path`), `s.moonAge` and `s.south` describe the moment. `s.sky` has the standard sky colors if you want them.
- Every 8 rows call `vTaskDelay(1)` and return `false` if `stop()` says so. The painter runs on core 0, whose idle task is watched: a loop that never yields reboots the device after 5 s.
- Do per-row work once per row, not per pixel, and avoid `sqrtf` where the squared distance will do. Moving Heat's per-pixel work to per-row took it from 840 to 585 ms.
- Use `dither565()` for the final pixel so gradients don't band on the 565 panel.
- Artwork that is easier to draw than to compute (palms, buildings) belongs in a generator script that writes both a C++ table and the SVG/JS for the faux display, so the two never drift. See `firmware/desk_buddy/tools/gen_heat.py`: it writes `src/heat_assets.cpp/.h` and `tools/heat_assets.js`, which is pasted between the `HEAT_ART begin/end` markers in `display.html`.

Declare the painter above `FACES` and set `.paint = paintMyFace`. The paint mutex is taken for you.

Faux display: `paint: (sc, F) => myFaceNodes(sc, F)` returns DOM/SVG nodes for the scene `sc` (`night`, `h`, `x`, `y` in 480 px, `age`, `keys`). Use a 480-unit SVG viewBox so the numbers match the firmware.

## A custom clock font

```sh
cd firmware/desk_buddy
python3 tools/gen_font.py 120 font_myface_120 " .0123456789:" --ttf MyFont.ttf --slant 0.12 --line 148,24
```

- Put the TTF (and its license) in `tools/fonts/`. Use a font whose license allows embedding (SIL OFL fonts from Google Fonts are fine).
- `--line 148,24` forces the standard clock's line height so Home's layout doesn't move. Pick the size so the digits are about 85-105 px tall.
- `--slant` fakes an italic. Digits are tabular, so the clock doesn't jiggle as the minutes change.
- Declare it in `home.cpp` with `LV_FONT_DECLARE(font_myface_120);` and set `.clockFont = &font_myface_120`. In the faux display add a `.clock.font-myface` CSS rule.

## Check it

1. Faux display first: `python3 -m http.server 8767 --bind 127.0.0.1` in the repo root, open `http://127.0.0.1:8767/display.html`. Set the time through `db.offset` in localStorage (`desk_buddy.v1`) to see morning, noon, sunset and night.
2. Build and flash (see `docs/testing.md`). The serial log prints `[home] HH:MM <face> day/night, background N ms` on every repaint: that's your paint time.
3. Screenshot it at several times: `tools/screenshot.py out.png --at 07:30`, `--at 12:00`, `--at 17:00`, `--at 21:00`.
4. Open the sheet (serial `U`) and walk the faces with the knob (`K1`). `firmware/desk_buddy/tools/sheetwalk.sh` does this and saves screenshots plus memory figures.
5. Turn the weather line and each stat off and on (sheet toggles): the column re-centers, so text moves over different parts of your background.

Known limit (Heat): with the weather line off, the stats ride up onto the sun, where white text is hard to read.
