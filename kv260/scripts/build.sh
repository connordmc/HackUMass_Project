#!/bin/bash
# Build faceshield_kv260 and run its unit tests. Run inside the Smart Camera
# container (scripts/start_container.sh). Pass --no-vitis to build without
# the DPU detector (only --detector full-frame then works).
set -euo pipefail
cd "$(dirname "$0")/.."

VITIS=ON
[ "${1:-}" = "--no-vitis" ] && VITIS=OFF

if ! pkg-config --exists opencv4 gstreamer-app-1.0 gstreamer-video-1.0 ||
   ! command -v cmake >/dev/null || ! command -v make >/dev/null ||
   ! command -v g++ >/dev/null ||
   ! [ -f /usr/include/google/protobuf/message.h ] ||
   ! [ -f /usr/include/glog/logging.h ]; then
  # protobuf and glog headers: included by the Vitis AI facedetect header.
  echo "Installing build dependencies..."
  # The container shares the host's /tmp, which the host may clean while apt
  # is unpacking; give apt a private temp dir. Also finish any install an
  # earlier interrupted run left half done.
  export TMPDIR=/var/tmp/faceshield-apt
  mkdir -p "$TMPDIR"
  export DEBIAN_FRONTEND=noninteractive
  dpkg --configure -a || true  # may fail until apt -f fetches missing parts
  apt-get -f install -y
  apt-get update
  apt-get install -y cmake make g++ pkg-config libopencv-dev \
    libprotobuf-dev libgoogle-glog-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
fi

# A cache from a configure that failed (e.g. no compiler yet) would be reused.
[ -f build/CMakeCache.txt ] && ! [ -f build/Makefile ] && rm -rf build
cmake -S . -B build -DFACESHIELD_WITH_VITIS="$VITIS"
cmake --build build -j"$(nproc)"
(cd build && ctest --output-on-failure)
echo "built: $(pwd)/build/faceshield_kv260"
