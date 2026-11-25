#!/bin/sh
# Build the firmware and merge bootloader, partition table, boot_app0 and app into one image
# that flashes at offset 0, for a full fresh install with esptool. It ERASES saved settings (the
# NVS area sits between the parts); the web flasher writes the parts separately instead.
# Usage: tools/merge_bin.sh [out.bin]   (from firmware/desk_buddy; default .pio/desk-buddy.bin)
# Needs PlatformIO (pio) and esptool 4.6+ (pip install esptool, or PlatformIO's own copy).
set -e
cd "$(dirname "$0")/.."
out=${1:-.pio/desk-buddy.bin}
PIO=${PIO:-pio}
command -v "$PIO" >/dev/null 2>&1 || PIO=~/.platformio/penv/bin/pio
"$PIO" run
b=.pio/build/crowpanel21
app0=$(find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32" -name boot_app0.bin | head -1)
if command -v esptool >/dev/null 2>&1; then ET="esptool"
elif command -v esptool.py >/dev/null 2>&1; then ET="esptool.py"
else ET="$HOME/.platformio/penv/bin/python $HOME/.platformio/packages/tool-esptoolpy/esptool.py"; fi
# Offsets: ESP32-S3 bootloader at 0x0, partition table 0x8000, otadata (boot_app0) 0xe000, app 0x10000
# (elecrow_s3.csv). Flash: QIO, 80 MHz, 16 MB.
$ET --chip esp32s3 merge-bin -o "$out" --flash-mode qio --flash-freq 80m --flash-size 16MB \
  0x0 "$b/bootloader.bin" 0x8000 "$b/partitions.bin" 0xe000 "$app0" 0x10000 "$b/firmware.bin"
echo "merged: $out ($(wc -c < "$out" | tr -d ' ') bytes)"
