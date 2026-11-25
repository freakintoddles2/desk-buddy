# Hardware

## The board

Elecrow CrowPanel 2.1" HMI ESP32 Rotary Display (480x480 round IPS, capacitive touch, rotary knob with press).

| Part | Detail |
|---|---|
| Chip | ESP32-S3 (dual core, 240 MHz), 8 MB PSRAM, 16 MB flash |
| Screen | 480x480 round IPS, RGB parallel panel, RGB565 framebuffer in PSRAM |
| Touch | CST816-family controller on I2C (SDA 38, SCL 39, address 0x15) |
| IO expander | PCF8574 at 0x21: P0 touch reset, P2 touch interrupt, P3 LCD power, P4 LCD reset, P5 knob switch (active low) |
| Knob | Quadrature encoder on GPIO 42 (A) and 4 (B), half-step: one detent is two transitions |
| Backlight | GPIO 6, 12-bit PWM |
| LED | GPIO 43 (also the UART1 plug's TX) |
| Plugs | USB-C (power + native USB serial), UART1 (4-pin), I2C (4-pin) |

Pins and the power-up sequence come from Elecrow's `Board21.h` and factory demo: <https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen>.

Board-specific fixes already in `board.cpp`:
- **Color order:** Elecrow's code writes MADCTL 0x08 (BGR); on these panels that swaps red and blue, so the firmware writes RGB.
- **Knob direction and half steps:** a step is counted at each resting state, and the sign is flipped to match the A/B wiring.
- **Touch offset:** taps landed a little low; `TOUCH_Y_SHIFT` (10 px) moves every touch up. If yours are off, change it.
- **Lift-off glitch:** on release the touch controller reports one last point at (0, 0). `readTouchRaw` treats (0, 0) as no touch (that corner is outside the round glass); without it a still tap could read as a 250 px drag.

## USB and flashing

- Use a USB-C data cable on the board's main USB port. It shows up as an Espressif "USB JTAG/serial debug unit" (VID 303A), for example `/dev/cu.usbmodem101` on a Mac (set `upload_port`/`monitor_port` in `platformio.ini` if yours differs).
- Plug straight into the computer, not through a monitor or hub, if it won't show up.
- The factory demo doesn't enable USB serial. For the first flash, hold **BOOT** while plugging in. After that, `pio run -t upload` resets the board itself.
- The partition table is `elecrow_s3.csv` (a 10 MB app partition; the default 3 MB is too small).
- To go back to the factory demo, flash Elecrow's merged binary from their repo (`factory_firmware/`) at 0x0 with esptool. Back up your own board first if you like: `esptool.py read_flash 0 0x1000000 backup.bin`.

## Memory budget

Measured with serial `M` (see `docs/testing.md`):
- **Internal RAM:** ~350 KB total, ~110 KB free at boot, ~80-100 KB in use. The fast travel tray borrows a few KB while open, the customize sheet's preview painter ~5 KB. This is the tight one.
- **PSRAM:** ~5.4 MB usable. ~2.8 MB free at boot, ~2.2-2.4 MB after visiting every panel. Big users: the framebuffer, two 460 KB background buffers per painted panel (Home, Weather), the Focus dial's 460 KB cache (released when Home's customize sheet opens), the sheet's previews (up to 4 x ~150 KB).
- **Flash:** the firmware is ~2.8 MB of the 10 MB app partition.

## Optional buzzer

A KY-006 *passive* buzzer module (the bare buzzer, silkscreen S / middle / -, no transistor) can run straight off the UART1 plug:

| Buzzer | UART1 plug |
|---|---|
| S | RX (GPIO 44) |
| - | GND |
| middle | not connected |

- Use RX, not TX: TX (GPIO 43) is also the LED.
- The plug's power pin is 5 V while the GPIO is 3.3 V, so modules with a transistor and a VCC pin (for example MH-FMD) don't work from this plug.
- The board box includes a 4-pin cable (MX1.25 to Dupont). Check the pin order against the board's silkscreen, not the wire colors.
- Leave the I2C plug alone: it shares the bus with touch.
- `buzzer.cpp` plays step sounds on a core-0 task. `sound-lab.html` designs sounds and can play them on the device over Web Serial (Chrome or Edge).

Event sounds live in `sounds.cpp`. The whole sound lab library (about 160 sounds, ~10 KB of flash) is built in, and every event can use any of it. Each event also has a default and a few suggestions. All of it comes from `sound-lab.html` (`CATALOG`, and `EVENTS` with `def` and `suggest`), turned into `src/sound_data.h` and the faux display's list by `node tools/gen_sounds.js`. To change a default, add a suggestion or add a sound, change the lab and rerun the generator. Picks are saved by the lab's ids (NVS `sndpick`), so reordering is safe; a removed id falls back to the default.

On the device, Settings > Sound has Volume (Off, Low, Medium, High), an on/off for the knob and typing clicks, and Sounds: every event with its sound. Opening one shows its suggestions, then each category of the library; turning through sounds plays each one. Clicks play a volume step below the setting, are rate-limited, and never cut into another sound.

`sound-lab.html` shows what each event plays, lets you pick from its suggestions or the whole library (with a timeline of whatever you're hearing), and has an editor for making new sounds. With the desk buddy connected (Chrome or Edge, USB), it plays sounds on the real buzzer. Sounds made there reach the device through the firmware code export for now.
