# Testing

## Tools

Run the Python tools with PlatformIO's Python (it has `pyserial`): `~/.platformio/penv/bin/python`. Only one program can hold the serial port: close `serial_log.py`, `pio device monitor` or the sound lab's Device card first.

| Tool | Use |
|---|---|
| `tools/screenshot.py out.png [--at HH:MM] [--now] [--send "W1,K2,P"] [--port P]` | Saves the framebuffer as a PNG. `--at` sets the clock to that time today first; `--send` sends test inputs first. Retry if the image comes back partly noisy. |
| `tools/serial_log.py [minutes] [port]` | Logs serial output and sets the device clock from the computer on each connect. |
| `tools/sheetwalk.sh <dir>` | Walks Home's customize sheet with the knob, saves screenshots and memory figures. Run before and after a change and compare. |
| `tools/snap.sh out.jpg [crop]` | A webcam frame of the device (macOS, ffmpeg), for things the framebuffer can't show (backlight level). |
| `tools/backlight_low.py` | Steps the backlight's raw PWM at the low end. |

## Serial commands

Send a line over USB serial (115200 baud):

| Command | Does |
|---|---|
| `K<n>` | Turn the knob n steps (negative = the other way) |
| `P` / `H` / `G` | Press / hold (back out) / the 3 s hold (go Home) |
| `W1` / `W-1` | Swipe to the next / previous panel |
| `U` / `V` | Swipe up from the bottom edge (Home's customize sheet) / swipe down |
| `D` / `Q` / `Y<n>` | Open or close the fast travel tray / tap its brightness button / scripted pull and flick |
| `S` | Screenshot (used by `screenshot.py`) |
| `M` | Memory: internal and PSRAM free, lowest, largest block; loop stack left |
| `T<epoch>` | Set the clock (Unix seconds) |
| `C0` / `C1` / `C2` | Time format auto / 12-hour / 24-hour |
| `F` | Fetch the weather now |
| `L<city>` / `L#<n>` | Search a weather location / pick result n |
| `N` | Drop Wi-Fi (tests reconnecting) |
| `A` / `AA` | Fire the water reminder / make every enabled reminder due |
| `B?` / `Bx<speed>[,HHMM]` / `BR<duty>` | Auto-brightness state / run its clock fast (`Bx0` stops) / raw backlight duty 0-4095 |
| `Z...` | Play a sound on the buzzer (sent by `sound-lab.html`) |
| `SND?` | Print volume, clicks and every event's sound, then `[sound] end` |
| `SNDP <event> <sound or ->` / `SNDC <event> <name>\|<repeat>,<gap>,<soft>:f0,f1,ms;...` | Pick a library sound for an event / save the event's own sound (the sound lab's Send) |
| `SNDV <0-3>` / `SNDK <0\|1>` / `SNDR` | Volume / knob clicks / reset every event to its default |

Touch can't be injected. Taps, drags, flicks and ring touches need a real finger.

## Logs worth knowing

- `[perf]` (when `PERF_LOG` is 1 in `board.cpp`): frame time and pixels drawn, once a second while anything redraws.
- `[home] HH:MM <face> day/night, background N ms`: each Home background paint.
- `[home] preview <face> in N ms`: customize-sheet previews.
- `[tray]`: timing breakdown when the fast travel tray closes.

## The faux display

Serve the repo root (`python3 -m http.server 8767 --bind 127.0.0.1`) and open `display.html`. Keyboard: arrows rotate and swipe, Enter presses, T opens the fast travel tray, U the customize sheet. State is in `localStorage['desk_buddy.v1']`; set `offset` (ms) to move its clock, for example to check a face at night:

```js
const db = JSON.parse(localStorage['desk_buddy.v1']);
const t = new Date(); t.setHours(21, 0, 0, 0);
db.offset = t - Date.now(); localStorage['desk_buddy.v1'] = JSON.stringify(db); location.reload();
```

## Checking a change that shouldn't change pixels

1. Before the change: screenshots of the affected screens at fixed times (`--at`), and for painters a hash of the painted buffer for fixed inputs (a temporary serial command that paints into a buffer and prints an FNV-1a hash works well).
2. After: the same, compared bit for bit. Differences in the clock, live weather text or a noisy capture are expected; anything else is a bug.
