# Spin the wheel (archived)

Removed from the desk buddy on 2026-11-09. It was one game in Play holding about
3.5 MB of PSRAM and ~60 KB of internal RAM from boot, whether or not it was ever opened.

Measured on the device:

| | Free PSRAM | Free internal RAM |
|---|---|---|
| With the wheel, at boot | 671 KB | 45 KB |
| With the wheel, after visiting every panel | 215 KB | 35 KB (3 KB lowest, tray open) |
| Without it, at boot | 2862 KB | 112 KB |
| Without it, after visiting every panel | 2406 KB | 103 KB |

## What's here

- `wheel.cpp`: the firmware (was `firmware/desk_buddy/src/wheel.cpp`). Look F2: a neon band of
  segments on the screen's edge, curved rim labels, the middle shows the option under the
  pointer. Rendering notes at the top of the file.
- `display_wheel.html`: the faux display's code (CSS, `WHEEL_SETS`/`WHEEL_COLORS`, `wheelMode`),
  with where each part went.
- `wheel-mockups.html`: the design mockups (F2 was picked).

## Bringing it back as it was

1. Move `wheel.cpp` back into `firmware/desk_buddy/src/`.
2. `play.cpp`: declare `Screen *makeWheelMode();` and `String wheelSetName();`, add
   `{"wheel", "Spin the wheel", "", wheelSetName()}` to `items()` (between coin and reflex), and
   `if (it->id == "wheel") app::push(makeWheelMode());` in `pick()`.
3. `main.cpp`: call `wheelPrepare()` after `tray::begin()`. It also renders Breathe's orb and
   the coin, so remove the "prep" task that now does that, or drop those two calls from
   `wheelPrepare()`.
4. `display.html`: paste the parts from `display_wheel.html` back and restore the Play entry.

## Bringing it back lean (recommended if it ever returns)

The memory went to lookup tables built for speed. A rebuild with the same look, ~250 KB only
while Spin is open:
- Compute each pixel's angle while drawing (small atan table) instead of ~820 KB of per-pixel
  tables.
- Draw the band straight into the framebuffer at rest as well as spinning (like the fast
  travel tray); LVGL draws only the middle. Drops the 450 KB screen image.
- The halo and thin ring are radial: 1-D falloff curves instead of 316x316 masks (~300 KB).
- One 8-bit palettized label texture instead of two RGB565 ones (~130 KB vs ~520 KB).
- Build on open (~0.5 s; show the colors first, fade labels in), free on close.
