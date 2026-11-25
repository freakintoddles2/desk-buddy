#!/usr/bin/env python3
"""Rasterize the panel-nav icons from ../../display.html into LVGL A8 images.

The faux display draws each icon as a tiny SVG (viewBox -3 -3 6 6, stroke .55, round caps).
LVGL can't draw SVG cheaply, so this script parses the handful of SVG features those icons
use (path M/L/H/V/C/A/Z, circle, ellipse, rect with rx) and renders them with Pillow at 8x,
then downsamples. A8 = alpha only, so the firmware recolors each icon with any color.

Usage: python3 tools/gen_icons.py [size_px]   (writes src/icons.c and src/icons.h)
"""
import math
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
HTML = ROOT.parent.parent / "display.html"
SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else 16
SS = 8                      # supersampling factor
STROKE = 0.55               # in viewBox units, from the faux display's CSS
VB = 6.0                    # viewBox is -3 -3 6 6


def icon_svgs():
    text = HTML.read_text()
    block = text[text.index("const ICONS = {"):]
    block = block[:block.index("};")]
    return dict(re.findall(r"(\w+): '(<svg.*?</svg>)'", block))


def attrs(tag):
    return dict(re.findall(r'([\w-]+)="([^"]*)"', tag))


def num(a, k, d=0.0):
    return float(a.get(k, d))


class Canvas:
    def __init__(self):
        self.px = SIZE * SS
        self.img = Image.new("L", (self.px, self.px), 0)
        self.d = ImageDraw.Draw(self.img)
        self.w = STROKE / VB * self.px

    def xy(self, x, y):
        return ((x + 3) / VB * self.px, (y + 3) / VB * self.px)

    def polyline(self, pts):
        if len(pts) < 2:
            return
        p = [self.xy(*q) for q in pts]
        self.d.line(p, fill=255, width=max(1, round(self.w)), joint="curve")
        r = self.w / 2
        for (x, y) in (p[0], p[-1]):
            self.d.ellipse((x - r, y - r, x + r, y + r), fill=255)

    def ellipse_pts(self, cx, cy, rx, ry, n=96):
        return [(cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n)) for i in range(n + 1)]

    def fill_poly(self, pts, value):
        self.d.polygon([self.xy(*q) for q in pts], fill=value)

    def rrect_pts(self, x, y, w, h, r, n=12):
        pts = []
        corners = [(x + w - r, y + r, -90), (x + w - r, y + h - r, 0), (x + r, y + h - r, 90), (x + r, y + r, 180)]
        for cx, cy, a0 in corners:
            for i in range(n + 1):
                a = math.radians(a0 + 90 * i / n)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        pts.append(pts[0])
        return pts


def arc_pts(x1, y1, rx, ry, phi, large, sweep, x2, y2, n=32):
    """SVG elliptical arc to points (endpoint → center parameterization, SVG spec F.6.5)."""
    phi = math.radians(phi)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p ** 2) / rx ** 2 + (y1p ** 2) / ry ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num_ = rx ** 2 * ry ** 2 - rx ** 2 * y1p ** 2 - ry ** 2 * x1p ** 2
    den = rx ** 2 * y1p ** 2 + ry ** 2 * x1p ** 2
    co = math.sqrt(max(0, num_ / den)) * (-1 if large == sweep else 1)
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    pts = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        pts.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return pts


def path_polylines(d):
    toks = re.findall(r"[MLHVCAZ]|-?\d*\.?\d+", d)
    i, cmd = 0, None
    cur, start = (0.0, 0.0), (0.0, 0.0)
    lines, pts = [], []

    def nxt():
        nonlocal i
        v = float(toks[i]); i += 1
        return v

    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]; i += 1
        if cmd == "M":
            if len(pts) > 1:
                lines.append(pts)
            cur = (nxt(), nxt()); start = cur; pts = [cur]; cmd = "L"
        elif cmd == "L":
            cur = (nxt(), nxt()); pts.append(cur)
        elif cmd == "H":
            cur = (nxt(), cur[1]); pts.append(cur)
        elif cmd == "V":
            cur = (cur[0], nxt()); pts.append(cur)
        elif cmd == "C":
            c1, c2, e = (nxt(), nxt()), (nxt(), nxt()), (nxt(), nxt())
            p0 = cur
            for k in range(1, 25):
                t = k / 24
                pts.append(tuple((1 - t) ** 3 * p0[j] + 3 * (1 - t) ** 2 * t * c1[j] + 3 * (1 - t) * t ** 2 * c2[j] + t ** 3 * e[j] for j in range(2)))
            cur = e
        elif cmd == "A":
            rx, ry, phi, large, sweep, x, y = nxt(), nxt(), nxt(), int(nxt()), int(nxt()), nxt(), nxt()
            pts.extend(arc_pts(cur[0], cur[1], rx, ry, phi, large, sweep, x, y))
            cur = (x, y)
        elif cmd == "Z":
            pts.append(start); cur = start
            lines.append(pts); pts = [cur]; cmd = None
    if len(pts) > 1:
        lines.append(pts)
    return lines


def render(svg):
    c = Canvas()
    for tag in re.findall(r"<(?:path|circle|ellipse|rect)[^>]*>", svg):
        kind = tag[1:].split()[0]
        a = attrs(tag)
        cls = a.get("class", "")
        if kind == "path":
            for pl in path_polylines(a["d"]):
                c.polyline(pl)
        elif kind in ("circle", "ellipse"):
            rx = num(a, "r") if kind == "circle" else num(a, "rx")
            ry = num(a, "r") if kind == "circle" else num(a, "ry")
            pts = c.ellipse_pts(num(a, "cx"), num(a, "cy"), rx, ry)
            if "face" in cls:
                c.fill_poly(pts, 255)
            else:
                if "knob" in cls:  # knob is filled with the screen color: punch out what's under it
                    c.fill_poly(pts, 0)
                c.polyline(pts)
        elif kind == "rect":
            pts = c.rrect_pts(num(a, "x"), num(a, "y"), num(a, "width"), num(a, "height"), num(a, "rx"))
            c.polyline(pts)
    return c.img.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    icons = icon_svgs()
    out_c = ["/* Generated by tools/gen_icons.py from display.html. Do not edit. */",
             '#include "icons.h"', ""]
    out_h = ["/* Generated by tools/gen_icons.py from display.html. Do not edit. */", "#pragma once",
             "#include <lvgl.h>", "", f"#define ICON_SIZE {SIZE}", ""]
    for name, svg in icons.items():
        data = list(render(svg).tobytes())
        rows = [", ".join(f"0x{v:02x}" for v in data[r * SIZE:(r + 1) * SIZE]) for r in range(SIZE)]
        out_c.append(f"static const uint8_t icon_{name}_map[] = {{")
        out_c += [f"  {r}," for r in rows]
        out_c.append("};")
        out_c.append(f"const lv_image_dsc_t icon_{name} = {{")
        out_c.append(f"  .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .flags = 0, .w = {SIZE}, .h = {SIZE}, .stride = {SIZE} }},")
        out_c.append(f"  .data_size = sizeof(icon_{name}_map),")
        out_c.append(f"  .data = icon_{name}_map,")
        out_c.append("};")
        out_c.append("")
        out_h.append(f"extern const lv_image_dsc_t icon_{name};")
    (ROOT / "src" / "icons.c").write_text("\n".join(out_c) + "\n")
    (ROOT / "src" / "icons.h").write_text("\n".join(out_h) + "\n")
    # Preview sheet for eyeballing.
    sheet = Image.new("L", (len(icons) * (SIZE * 4 + 8), SIZE * 4), 0)
    for k, svg in enumerate(icons.values()):
        sheet.paste(render(svg).resize((SIZE * 4, SIZE * 4), Image.NEAREST), (k * (SIZE * 4 + 8), 0))
    sheet.save(ROOT / "tools" / "icons_preview.png")
    print(f"{len(icons)} icons at {SIZE}px: {', '.join(icons)}")


if __name__ == "__main__":
    main()
