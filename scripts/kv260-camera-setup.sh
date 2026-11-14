#!/usr/bin/env bash
# Run on the KV260 after every boot: loads the Smart Camera firmware and sets
# the IAS camera chain (AR1335 -> AP1302 -> CSI-2 RX -> /dev/video0) to
# 1920x1080 YUV 4:2:0, then checks that frames actually arrive.
set -euo pipefail

W=1920
H=1080
CODE=0x2100  # VYYUYY8_1X24 (YUV 4:2:0); media-ctl can't parse it, so use v4l2-ctl

sudo xmutil unloadapp >/dev/null 2>&1 || true  # errors if nothing is loaded
sudo xmutil loadapp kv260-smartcam
sleep 2  # let the camera drivers probe

v4l2-ctl -d /dev/v4l-subdev2 --set-subdev-fmt pad=2,width=$W,height=$H,code=$CODE,field=none
v4l2-ctl -d /dev/v4l-subdev0 --set-subdev-fmt pad=0,width=$W,height=$H,code=$CODE,field=none
v4l2-ctl -d /dev/v4l-subdev0 --set-subdev-fmt pad=1,width=$W,height=$H,code=$CODE,field=none
v4l2-ctl -d /dev/video0 --set-fmt-video=width=$W,height=$H,pixelformat=NV12

echo "Checking for frames (each '<' is one frame)..."
if timeout 15 v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=10 --stream-to=/dev/null; then
  echo
  echo "Camera OK. Start the stream with:"
  echo "  .venv/bin/python -m faceshield.stream --gst \"v4l2src device=/dev/video0 io-mode=mmap ! video/x-raw,format=NV12,width=$W,height=$H\""
else
  echo
  echo "No frames. Check the sensor with:"
  echo "  journalctl -k -b | grep -E 'NACK|ap1302' | tail"
  echo "'NACK 0x6c' means the AR1335 sensor is not answering: power off and reseat"
  echo "the ribbon cable on J7 (IAS0), contacts facing the right way, latches closed."
  exit 1
fi
