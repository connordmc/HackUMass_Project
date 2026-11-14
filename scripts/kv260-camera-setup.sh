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

# The AP1302 ISP loads its firmware after loadapp returns, so its device node
# appears several seconds later. Device numbers can also change, so look the
# nodes up by entity name in the media graph.
node() {  # prints the /dev node of the media entity whose name matches $1
  # Empty (not an error) while /dev/media0 doesn't exist yet: under
  # set -e/pipefail a failing lookup would otherwise end the script.
  { media-ctl -d /dev/media0 -p 2>/dev/null || true; } |
    awk -v pat="$1" '/^- entity/ {hit = ($0 ~ pat)} hit && /device node name/ {print $4; exit}'
}
echo -n "Waiting for the camera drivers"
for _ in $(seq 30); do
  ISP=$(node 'ap1302') CSI=$(node 'csiss') VIDEO=$(node 'vcap')
  [ -n "$ISP" ] && [ -n "$CSI" ] && [ -n "$VIDEO" ] && [ -e "$ISP" ] && [ -e "$VIDEO" ] && break
  echo -n "."
  sleep 1
done
echo
if [ -z "$ISP" ] || [ -z "$CSI" ] || [ -z "$VIDEO" ]; then
  echo "Camera drivers did not appear within 30 s. media graph:"
  media-ctl -d /dev/media0 -p 2>&1 | grep -E 'entity|device node' || true
  exit 1
fi
echo "ISP $ISP, CSI-2 RX $CSI, capture $VIDEO"

v4l2-ctl -d "$ISP" --set-subdev-fmt pad=2,width=$W,height=$H,code=$CODE,field=none
v4l2-ctl -d "$CSI" --set-subdev-fmt pad=0,width=$W,height=$H,code=$CODE,field=none
v4l2-ctl -d "$CSI" --set-subdev-fmt pad=1,width=$W,height=$H,code=$CODE,field=none
v4l2-ctl -d "$VIDEO" --set-fmt-video=width=$W,height=$H,pixelformat=NV12

echo "Checking for frames (each '<' is one frame)..."
if timeout 15 v4l2-ctl -d "$VIDEO" --stream-mmap=4 --stream-count=10 --stream-to=/dev/null; then
  echo
  echo "Camera OK. Start the stream with:"
  echo "  .venv/bin/python -m faceshield.stream --gst \"v4l2src device=$VIDEO io-mode=mmap ! video/x-raw,format=NV12,width=$W,height=$H\""
else
  echo
  echo "No frames. Check the sensor with:"
  echo "  journalctl -k -b | grep -E 'NACK|ap1302' | tail"
  echo "'NACK 0x6c' means the AR1335 sensor is not answering: power off and reseat"
  echo "the ribbon cable on J7 (IAS0), contacts facing the right way, latches closed."
  exit 1
fi
