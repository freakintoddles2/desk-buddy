#!/usr/bin/env python3
"""Steps the backlight through low duty levels so you can judge the dimmest usable setting.
Run: ~/.platformio/penv/bin/python tools/backlight_low.py [seconds per level]
Serial logger must be stopped. Restores 80% at the end (or on Ctrl-C)."""
import sys, time, serial

PORT = "/dev/cu.usbmodem101"
HOLD = float(sys.argv[1]) if len(sys.argv) > 1 else 5
# (label, raw 12-bit duty)
LEVELS = [("80%", 3276), ("30%", 1229), ("10%", 410), ("5%", 205), ("3%", 123), ("2%", 82),
          ("1%", 41), ("0.5%", 20), ("0.25%", 10), ("0.1%", 4), ("1/4095", 1), ("off", 0)]

s = serial.Serial(PORT, 115200, timeout=0.2)
time.sleep(0.3)
def duty(d): s.write(f"BR{d}\n".encode()); s.flush()
try:
    for label, d in LEVELS:
        duty(d)
        print(f"{label:>7}  (duty {d})  - watch for flicker, and whether the clock is still readable", flush=True)
        time.sleep(HOLD)
finally:
    duty(3276)
    print("back to 80%")
