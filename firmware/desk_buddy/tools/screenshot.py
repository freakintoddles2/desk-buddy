#!/usr/bin/env python3
"""Save the desk buddy's screen as a PNG, straight from the panel's framebuffer over USB serial.

Usage: ~/.platformio/penv/bin/python tools/screenshot.py <out.png> [--at HH:MM] [--now] [--port P]
  --at HH:MM  set the device clock to that time today first (e.g. --at 21:00 for the night look)
  --now       set the device clock to the Mac's time first
  --send CMDS comma-separated test inputs sent first, e.g. "W-1,K2,P" (K<n> knob, P press,
              H hold, W<+1|-1> swipe), 0.5 s apart
The serial logger (tools/serial_log.py) must not be holding the port.
"""
import argparse
import datetime
import sys
import time

import struct
import zlib

import serial

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--at")
ap.add_argument("--now", action="store_true")
ap.add_argument("--send", default="")
ap.add_argument("--port", default="/dev/cu.usbmodem101")
args = ap.parse_args()

s = serial.Serial(args.port, 115200, timeout=8)  # panel interrupt on core 0 slows USB serial
time.sleep(0.3)
s.reset_input_buffer()

if args.at or args.now:
    t = datetime.datetime.now()
    if args.at:
        h, m = map(int, args.at.split(":"))
        t = t.replace(hour=h, minute=m, second=0, microsecond=0)
    s.write(f"T{int(t.timestamp())}\n".encode())
    time.sleep(1.5)  # the face redraws on the next tick
    s.reset_input_buffer()

for cmd in [c.strip() for c in args.send.split(",") if c.strip()]:
    s.write(f"{cmd}\n".encode())
    time.sleep(0.5)
time.sleep(0.4)
s.reset_input_buffer()

s.write(b"S\n")
deadline = time.time() + 10
header = b""
while not header.startswith(b"SNAP "):
    header = s.readline().strip()
    if time.time() > deadline:
        sys.exit("no SNAP header from the device")
_, w, h = header.split()
w, h = int(w), int(h)
data = b""
deadline = time.time() + 20
while len(data) < w * h * 2 and time.time() < deadline:
    data += s.read(w * h * 2 - len(data))
if len(data) != w * h * 2:
    sys.exit(f"short read: {len(data)} of {w * h * 2} bytes")

# RGB565 little-endian -> 8-bit RGB, written as a PNG with only the standard library
# (PlatformIO's Python, which has pyserial, doesn't have Pillow).
rows = bytearray()
for y in range(h):
    rows.append(0)  # PNG filter: none
    for x in range(w):
        i = (y * w + x) * 2
        v = data[i] | (data[i + 1] << 8)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
with open(args.out, "wb") as f:
    f.write(png)
print(f"saved {args.out} ({w}x{h})")
