// The moon at its real phase (display.html moonNodes): the lit part from today's phase, pale
// maria where they really are, and the dark part shown faintly (earthshine) so a new moon is
// still a moon. South of the equator the moon is seen upside down, so the lit side and the
// maria flip; the hemisphere comes from the weather location.
#pragma once

#include <lvgl.h>
#include <time.h>

#include "dither.h"

namespace moon {

// Age through the month, 0..1: 0 new, .25 first quarter, .5 full, .75 last quarter. From the
// mean 29.53-day month since a known new moon (6 Jan 2000 18:14 UTC); within about half a day.
float age(time_t utc);
// Fraction of the disc lit, 0..1.
float lit(float age);
// True south of the equator (weather location, else the device zone's city).
bool south();

// One pixel of the moon, no glow. (u, v): the pixel's offset from the moon's center in moon
// radii (v down); aa: one pixel in moon radii (antialiasing). Returns the moon's color and its
// coverage over whatever is behind it (0 outside the disc).
struct Px { dither::RGB c; float a; };
Px pixel(float u, float v, float aa, float age, bool south);

// Home's moon sprite (ARGB8888, 72 px) for a radius in px, redrawn only when the phase,
// hemisphere, radius or glow changes. glow: Sun ring's soft halo around the lit part (Living
// sky has none; its sky glow is painted into the background). Lives in PSRAM (~20 KB).
const lv_image_dsc_t *sprite(float radius, bool glow);

}  // namespace moon
