#!/usr/bin/env python3
"""Weather condition icons for the Weather panel and Home (display.html wxIcon), as LVGL ARGB8888.

The faux display draws them as SVG in a 24x24 box: a yellow sun with 8 rays, a pale crescent
moon, a cloud, blue rain streaks. Pillow can't read SVG, so the same shapes are rebuilt here from
circles and lines (the cloud outline is the union of its three arcs plus a flat base), drawn at 8x
and scaled down. Snow, storm and fog are additions in the same style (the faux display only has
clear, partly cloudy, cloudy and rain).

Usage: python3 tools/gen_wx_icons.py   (writes src/wx_icons.c, src/wx_icons.h, tools/wx_icons_preview.png)
"""
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SIZES = {"lg": 80, "sm": 32}
SS = 8
VB = (-1.0, -1.0, 26.0)  # x, y, size: every icon uses the rain icon's taller box so all match

SUN = (0xF5, 0xB9, 0x42)
MOON = (0xC9, 0xD3, 0xFF)
BLUE = (0x4F, 0x8C, 0xFF)


class Icon:
    def __init__(self, size):
        self.size = size
        self.px = size * SS
        self.img = Image.new("RGBA", (self.px, self.px), (0, 0, 0, 0))

    def s(self, v):
        return v / VB[2] * self.px

    def xy(self, x, y):
        return (self.s(x - VB[0]), self.s(y - VB[1]))

    def paste(self, mask, rgb):
        layer = Image.new("RGBA", self.img.size, rgb + (255,))
        layer.putalpha(mask)
        self.img = Image.alpha_composite(self.img, layer)

    def mask(self):
        return Image.new("L", self.img.size, 0)

    def circle(self, m, cx, cy, r, fill=255):
        x, y = self.xy(cx, cy)
        r = self.s(r)
        ImageDraw.Draw(m).ellipse((x - r, y - r, x + r, y + r), fill=fill)

    def line(self, m, x1, y1, x2, y2, w):
        d = ImageDraw.Draw(m)
        p1, p2 = self.xy(x1, y1), self.xy(x2, y2)
        w = self.s(w)
        d.line([p1, p2], fill=255, width=max(1, round(w)))
        for (x, y) in (p1, p2):
            d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=255)

    def sun(self, cx, cy, r):
        m = self.mask()
        self.circle(m, cx, cy, r)
        import math
        for i in range(8):
            a = i / 8 * 2 * math.pi
            self.line(m, cx + (r + 1.6) * math.cos(a), cy + (r + 1.6) * math.sin(a),
                      cx + (r + 3) * math.cos(a), cy + (r + 3) * math.sin(a), 1.4)
        self.paste(m, SUN)

    def moon(self, cx, cy, r):
        m = self.mask()
        self.circle(m, cx, cy, r)
        self.circle(m, cx + 0.5 * r, cy - 0.3 * r, 0.8 * r, fill=0)
        self.paste(m, MOON)

    def cloud(self, rgb, dy=0.0):
        # M7 19.5h10.2 a4.3 (right lobe) a6 (top) A3.5 (left lobe) Z
        m = self.mask()
        self.circle(m, 12.08, 13.24 + dy, 6)
        self.circle(m, 17.4, 15.2 + dy, 4.3)
        self.circle(m, 7.24, 16.0 + dy, 3.5)
        x0, y0 = self.xy(7.24, 13.5 + dy)
        x1, y1 = self.xy(17.4, 19.5 + dy)
        ImageDraw.Draw(m).rectangle((x0, y0, x1, y1), fill=255)
        # flat base
        _, yb = self.xy(0, 19.5 + dy)
        ImageDraw.Draw(m).rectangle((0, yb, self.px, self.px), fill=0)
        self.paste(m, rgb)

    def streaks(self, pts, rgb, dx=-1.2, dy=2.6, w=1.6):
        m = self.mask()
        for x, y in pts:
            self.line(m, x, y, x + dx, y + dy, w)
        self.paste(m, rgb)

    def dots(self, pts, rgb, r=1.0):
        m = self.mask()
        for x, y in pts:
            self.circle(m, x, y, r)
        self.paste(m, rgb)

    def bolt(self, rgb):
        m = self.mask()
        pts = [(12.6, 17.2), (9.6, 21.6), (12.0, 21.6), (10.6, 25.0), (14.6, 20.2), (12.2, 20.2), (13.8, 17.2)]
        ImageDraw.Draw(m).polygon([self.xy(*p) for p in pts], fill=255)
        self.paste(m, rgb)

    def done(self):
        return self.img.resize((self.size, self.size), Image.LANCZOS)


def draw(kind, size):
    i = Icon(size)
    if kind == "clear_day":
        i.sun(12, 12, 5)
    elif kind == "clear_night":
        i.moon(12, 12, 7)
    elif kind == "partly_day":
        i.sun(9, 8, 3.8)
        i.cloud((0xDF, 0xE5, 0xEF))
    elif kind == "partly_night":
        i.moon(9, 8, 5.5)
        i.cloud((0xDF, 0xE5, 0xEF))
    elif kind == "cloudy":
        i.cloud((0xB9, 0xC2, 0xD0))
    elif kind == "fog":
        i.cloud((0x8B, 0x93, 0xA3), dy=-2.5)
        m = i.mask()
        for y, x0, x1 in ((19.6, 4.5, 19.5), (22.6, 6.5, 21.5)):
            i.line(m, x0, y, x1, y, 1.5)
        i.paste(m, (0xB9, 0xC2, 0xD0))
    elif kind == "rain":
        i.cloud((0x9A, 0xA6, 0xB8))
        i.streaks([(8, 21), (12, 22), (16, 21)], BLUE)
    elif kind == "snow":
        i.cloud((0xB9, 0xC2, 0xD0))
        i.dots([(7.5, 22), (12, 23.2), (16.5, 22)], (0xF2, 0xF4, 0xF8), r=1.15)
    elif kind == "storm":
        i.cloud((0x7E, 0x88, 0x9A), dy=-1.5)
        i.bolt(SUN)
    return i.done()


KINDS = ["clear_day", "clear_night", "partly_day", "partly_night", "cloudy", "fog", "rain", "snow", "storm"]


def emit(name, img):
    w, h = img.size
    rgba = img.tobytes()
    data = bytearray(len(rgba))
    data[0::4], data[1::4], data[2::4], data[3::4] = rgba[2::4], rgba[1::4], rgba[0::4], rgba[3::4]  # B, G, R, A
    rows = [", ".join(f"0x{v:02x}" for v in data[i:i + 32]) for i in range(0, len(data), 32)]
    return (f"static const uint8_t {name}_map[] = {{\n  " + ",\n  ".join(rows) + "\n};\n"
            f"const lv_image_dsc_t {name} = {{\n"
            f"  .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .flags = 0, .w = {w}, .h = {h}, .stride = {w * 4} }},\n"
            f"  .data_size = sizeof({name}_map),\n  .data = {name}_map,\n}};\n")


def main():
    c = ["/* Generated by tools/gen_wx_icons.py. Do not edit. */", '#include "wx_icons.h"', ""]
    hdr = ["/* Generated by tools/gen_wx_icons.py. Do not edit. */", "#pragma once", "#include <lvgl.h>", ""]
    imgs = {}
    for sz, px in SIZES.items():
        for k in KINDS:
            img = draw(k, px)
            imgs[(sz, k)] = img
            c.append(emit(f"wx_{k}_{sz}", img))
            hdr.append(f"extern const lv_image_dsc_t wx_{k}_{sz};")
    (ROOT / "src" / "wx_icons.c").write_text("\n".join(c))
    (ROOT / "src" / "wx_icons.h").write_text("\n".join(hdr) + "\n")
    lg, sm = SIZES["lg"], SIZES["sm"]
    prev = Image.new("RGBA", (len(KINDS) * (lg + 12) + 12, lg + sm + 36), (0x0B, 0x0D, 0x11, 255))
    for n, k in enumerate(KINDS):
        x = 12 + n * (lg + 12)
        prev.alpha_composite(imgs[("lg", k)], (x, 12))
        prev.alpha_composite(imgs[("sm", k)], (x + (lg - sm) // 2, lg + 24))
    prev.resize((prev.width * 2, prev.height * 2), Image.NEAREST).save(ROOT / "tools" / "wx_icons_preview.png")
    print("ok")


if __name__ == "__main__":
    main()
