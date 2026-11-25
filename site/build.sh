#!/bin/sh
# Assemble the GitHub Pages site into _site/: these pages, the sound lab, the faux display, and
# the firmware parts plus manifest.json for the web flasher (ESP Web Tools).
# Run after building the firmware (pio run -d firmware/desk_buddy). VERSION overrides the label.
# Try it locally: site/build.sh && python3 -m http.server 8080 -d _site   (Install needs
# http://localhost or https, and Chrome or Edge).
set -e
cd "$(dirname "$0")/.."
out=_site
b=firmware/desk_buddy/.pio/build/crowpanel21
[ -f "$b/firmware.bin" ] || { echo "Build the firmware first: pio run -d firmware/desk_buddy" >&2; exit 1; }
app0=$(find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32" -name boot_app0.bin | head -1)
rm -rf "$out"
mkdir -p "$out/firmware"
cp site/*.html site/*.css "$out/"
cp -R site/img "$out/"
cp sound-lab.html display.html "$out/"
cp "$b/bootloader.bin" "$b/partitions.bin" "$b/firmware.bin" "$app0" "$out/firmware/"
version=${VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
# The parts go to their own offsets (elecrow_s3.csv), so an update that doesn't erase keeps the
# saved settings in NVS (0x9000).
cat > "$out/manifest.json" <<JSON
{
  "name": "Desk Buddy",
  "version": "$version",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "firmware/bootloader.bin", "offset": 0 },
        { "path": "firmware/partitions.bin", "offset": 32768 },
        { "path": "firmware/boot_app0.bin", "offset": 57344 },
        { "path": "firmware/firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
JSON
echo "site: $out (firmware $version)"
