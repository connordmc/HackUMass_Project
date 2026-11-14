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

echo "== GStreamer elements"
for e in mediasrcbin omxh264enc h264parse mpegtsmux tcpserversink rtph264pay appsrc appsink; do
  if gst-inspect-1.0 "$e" >/dev/null 2>&1; then pass "$e"; else fail "$e missing"; fi
done

echo "== DPU and model"
if have xdputil; then
  if xdputil query 2>/dev/null | grep -qi 'DPU'; then pass "DPU visible to xdputil"; else fail "xdputil sees no DPU"; fi
else
  warn "xdputil not found; skipped DPU query"
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
