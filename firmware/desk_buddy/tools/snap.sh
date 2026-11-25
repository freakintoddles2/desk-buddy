#!/bin/zsh
# Grab a frame of the desk buddy from a webcam pointed at it (set up for a Logitech C920).
# Usage: tools/snap.sh <out.jpg> [crop w:h:x:y]   (crop is in 1920x1080 frame pixels)
# The default crop is for one particular desk setup; measure your own (crop w:h:x:y).
out=${1:?output path}
crop=${2:-420:400:660:90}
ffmpeg -hide_banner -loglevel error -f avfoundation -pixel_format uyvy422 -framerate 30 \
  -video_size 1920x1080 -i "HD Pro Webcam C920" -ss 1.5 -frames:v 1 \
  -vf "crop=${crop},scale=iw*2:ih*2" -y "$out"
