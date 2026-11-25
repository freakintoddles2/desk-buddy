// Colors and sizes from display.html's CSS (--screen, --ink, --accent, ...).
#pragma once

#include <lvgl.h>

namespace theme {

inline lv_color_t screen() { return lv_color_hex(0x0B0D11); }
inline lv_color_t ink() { return lv_color_hex(0xF2F4F8); }
inline lv_color_t inkDim() { return lv_color_hex(0x8B93A3); }
inline lv_color_t inkFaint() { return lv_color_hex(0x4A505C); }
inline lv_color_t accent() { return lv_color_hex(0x4F8CFF); }
inline lv_color_t ok() { return lv_color_hex(0x3FCF7F); }
inline lv_color_t today() { return lv_color_hex(0xF5B942); }
inline lv_color_t overdue() { return lv_color_hex(0xFF6B6B); }

// Panel nav strip. Sized up from the faux display (top 10.5%, 50% wide) for the real screen:
// 280 px is about the width of the circle at that height; the edges fade out anyway.
constexpr int NAV_Y = 32;  // window 64 tall around the same centre (y 64) so the current icon's glow isn't clipped
constexpr int NAV_W = 280;
constexpr int NAV_H = 64;
constexpr int NAV_PILL_H = 34;
constexpr int NAV_GAP = 14;

}  // namespace theme
