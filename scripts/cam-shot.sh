#!/bin/bash
# Shoot the Korvo-1 board with the BRIO webcam (/dev/video0).
# Usage: tools/cam-shot.sh [focus] [name]
#   focus  manual focus_absolute (0-255 step 5). Default 75: the board
#          sits ~40-60 cm out and the lens maps larger values to FARTHER
#          focus (150+ blurs the screen, background sharp). 75 was scanned
#          2026-10-23 as the sharpest across the whole panel. "auto" keeps
#          continuous autofocus (center weighted; needs ~10 s to settle).
#   name   short label for the file (default "shot")
# Output: shots/<YYYYMMDDHHMMSS>-<name>.png  (timestamp prefix for the
# human to tell frames apart)
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
FOCUS=${1:-75}
NAME=${2:-shot}
TS=$(date +%Y%m%d%H%M%S)
OUT=$REPO/shots/$TS-$NAME.png
mkdir -p "$REPO/shots"

if [ "$FOCUS" = auto ]; then
    v4l2-ctl -d /dev/video0 -c focus_automatic_continuous=1
    sleep 10
else
    v4l2-ctl -d /dev/video0 -c focus_automatic_continuous=0
    v4l2-ctl -d /dev/video0 -c focus_absolute="$FOCUS"
fi

# BRIO needs ~2 s after stream start to settle (exposure + focus).
ffmpeg -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
    -video_size 1280x720 -i /dev/video0 -frames:v 30 -update 1 \
    -y "$OUT" 2>/dev/null || \
ffmpeg -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
    -video_size 1280x720 -i /dev/video0 -frames:v 1 -y "$OUT"
ls -la "$OUT"
