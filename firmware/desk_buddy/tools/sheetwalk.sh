#!/bin/zsh
# Walk Home's customize sheet with the knob over serial and save screenshots plus memory figures.
# Usage: tools/sheetwalk.sh <outdir>   (from firmware/desk_buddy; the device on /dev/cu.usbmodem101)
# Run it before and after a change to the sheet or the faces and compare the two folders.
# Inputs go over serial once each (screenshot.py's --send would repeat them on a retry).
# It changes the saved Home face (it ends on face 0) and sets the clock to a fixed time, then
# back to this Mac's time.
out=${1:?output folder}; mkdir -p $out
here=${0:A:h}
S=$here/screenshot.py; P=~/.platformio/penv/bin/python; PORT=${PORT:-/dev/cu.usbmodem101}
send(){ $P -c "
import serial,time,sys
s=serial.Serial(sys.argv[2],115200,timeout=0.5); time.sleep(0.3)
for c in sys.argv[1].split(','): s.write((c+'\n').encode()); time.sleep(0.6)
" "$1" $PORT; sleep ${2:-1.5}; }
shot(){ for i in 1 2 3 4; do $P $S "$@" --port $PORT >/dev/null 2>&1 && return 0; sleep 2; done; echo "FAILED $1"; }
send "G" 2; send "U" 3; send "K-20,P" 3      # Home, open the sheet, first face, close
send "T1795600800" 1                         # a fixed clock, so the text is the same each run
send "U" 3; shot $out/w0.png                  # sheet open on face 0
for i in 1 2 3 4 5; do send "K1"; shot $out/w$i.png; done   # first turn shows the selection, then faces, then toggles
send "K-4"; shot $out/w6.png
$P - $PORT > $out/mem.txt <<'PY'
import serial,time,sys
s=serial.Serial(sys.argv[1],115200,timeout=0.5); time.sleep(0.5); s.reset_input_buffer(); s.write(b'M\n')
end=time.time()+2
while time.time()<end:
    l=s.readline().decode(errors='replace').rstrip()
    if l.startswith('[mem]'): print(l)
PY
send "V" 1; send "G" 2; send "U" 3; send "K-20,P" 2
send "T$(date +%s)" 0
