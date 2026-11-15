#!/bin/bash
# Checks everything faceshield_kv260 needs on the KV260. Run it on the board
# (inside the Smart Camera container for the build/runtime checks).
# Prints PASS / WARN / FAIL per item; exits nonzero if anything failed.

MEDIA=${1:-/dev/media0}
MODEL=/opt/xilinx/kv260-smartcam/share/vitis_ai_library/models/densebox_640_360/densebox_640_360.xmodel
fails=0
pass() { printf '  PASS  %s\n' "$1"; }
warn() { printf '  WARN  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
have() { command -v "$1" >/dev/null 2>&1; }

echo "== Firmware and camera"
if have xmutil; then
  if xmutil listapps 2>/dev/null | grep -E 'kv260-smartcam' | grep -qE '[[:space:]]0[[:space:]]*$|,[[:space:]]*0'; then
    pass "kv260-smartcam firmware is loaded"
  else
    warn "could not confirm kv260-smartcam is the active app; output of xmutil listapps:"
    xmutil listapps 2>/dev/null | sed 's/^/          /'
  fi
else
  warn "xmutil not found (normal inside the container; check on the host)"
fi
if [ -e "$MEDIA" ]; then pass "$MEDIA exists"; else fail "$MEDIA missing: load the smartcam firmware"; fi
if have media-ctl && [ -e "$MEDIA" ]; then
  if media-ctl -d "$MEDIA" -p 2>/dev/null | grep -qiE 'ap1302|ar1335'; then
    pass "AP1302 ISP / AR1335 sensor found in the media graph"
  else
    fail "AR1335 / AP1302 not in media graph: check the IAS camera ribbon and connector"
  fi
else
  warn "media-ctl not available; skipped media graph check"
fi
# The AP1302 ISP is on the carrier board, so it appears in the media graph
# even when the AR1335 sensor behind it is unplugged or unpowered. The ISP
# logs "NACK 0x6c" when the sensor does not answer on I2C (seen when a capture
# is attempted), so check the kernel log as well.
if [ -f /.dockerenv ]; then
  warn "inside the container the host kernel log is not visible; run this script on the host too to check the AR1335 sensor"
elif have journalctl; then
  if journalctl -k -b --no-pager 2>/dev/null | grep -q 'NACK 0x6c'; then
    fail "AR1335 sensor is not answering the AP1302 (NACK 0x6c in the kernel log): power off, reseat the ribbon cable on J7 (IAS0), contacts facing the connector's contacts, latches closed"
  else
    pass "no AR1335 I2C errors in the kernel log since boot (only meaningful after a capture attempt)"
  fi
else
  warn "journalctl not available (normal inside the container); run this on the host to check the sensor"
fi

echo "== GStreamer elements"
for e in mediasrcbin omxh264enc h264parse mpegtsmux tcpserversink rtph264pay appsrc appsink; do
  if gst-inspect-1.0 "$e" >/dev/null 2>&1; then pass "$e"; else fail "$e missing"; fi
done

echo "== DPU and model"
# xdputil is a Python tool; in the smartcam 2022.1 container it crashes on a
# missing numpy, which says nothing about the DPU. Only trust a clean run.
dpu_out=$(xdputil query 2>&1)
if have xdputil && ! printf '%s' "$dpu_out" | grep -q 'Traceback'; then
  if printf '%s' "$dpu_out" | grep -qi 'DPU'; then pass "DPU visible to xdputil"; else fail "xdputil sees no DPU"; fi
else
  warn "xdputil unavailable or crashed; checking the DPU firmware instead"
  xclbin=$(sed -n 's/^firmware:[[:space:]]*//p' /etc/vart.conf 2>/dev/null)
  if [ -n "$xclbin" ] && [ -f "$xclbin" ]; then pass "DPU firmware $xclbin present"; else fail "DPU firmware (xclbin from /etc/vart.conf) not found"; fi
  if ls /dev/dri/renderD* >/dev/null 2>&1; then pass "DRM render node for the DPU present"; else fail "no /dev/dri/renderD* (zocl not loaded?)"; fi
fi
[ -e /etc/vart.conf ] && pass "/etc/vart.conf present" || warn "/etc/vart.conf missing (mount it into the container)"
[ -f "$MODEL" ] && pass "densebox_640_360 model present" || fail "model not found at $MODEL"

echo "== Build dependencies"
if find /usr/include /usr/local/include -path '*vitis/ai/facedetect.hpp' 2>/dev/null | grep -q .; then
  pass "Vitis AI Library facedetect header"
else
  fail "vitis/ai/facedetect.hpp not found (install the Vitis AI Library dev files)"
fi
if ldconfig -p 2>/dev/null | grep -q 'libvitis_ai_library-facedetect'; then
  pass "libvitis_ai_library-facedetect"
else
  fail "libvitis_ai_library-facedetect not found"
fi
for pc in opencv4 gstreamer-app-1.0 gstreamer-video-1.0; do
  pkg-config --exists "$pc" 2>/dev/null && pass "$pc dev files" || fail "$pc dev files missing"
done
have cmake && pass "cmake" || fail "cmake missing"

echo "== Network"
echo "  board addresses: $(hostname -I 2>/dev/null)"

if [ "$fails" -gt 0 ]; then echo "$fails check(s) failed"; exit 1; fi
echo "all required checks passed"
