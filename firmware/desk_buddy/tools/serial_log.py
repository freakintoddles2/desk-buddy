#!/usr/bin/env python3
"""Log the desk buddy's serial output, and set its clock from the Mac on connect.

Usage: ~/.platformio/penv/bin/python tools/serial_log.py [minutes] [port]
Reconnects if the board resets; sends "T<unix seconds>" each time it (re)connects.
"""
import sys
import time

import serial

MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 20
PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/cu.usbmodem101"


def stamp():
    return time.strftime("%H:%M:%S")


end = time.time() + MINUTES * 60
s = None
while time.time() < end:
    try:
        if s is None:
            s = serial.Serial(PORT, 115200, timeout=0.5)
            time.sleep(0.5)
            s.write(f"T{int(time.time())}\n".encode())
            print(stamp(), "[log] connected, sent time", flush=True)
        line = s.readline()
    except (serial.SerialException, OSError):
        if s is not None:
            print(stamp(), "[log] serial lost", flush=True)
        s = None
        time.sleep(1)
        continue
    if line:
        print(stamp(), line.decode(errors="replace").rstrip(), flush=True)
