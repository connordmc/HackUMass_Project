#!/bin/bash
# Build faceshield_kv260 and run its unit tests. Run inside the Smart Camera
# container (scripts/start_container.sh). Pass --no-vitis to build without
# the DPU detector (only --detector full-frame then works).
set -euo pipefail
cd "$(dirname "$0")/.."

VITIS=ON
[ "${1:-}" = "--no-vitis" ] && VITIS=OFF

if ! pkg-config --exists opencv4 gstreamer-app-1.0 gstreamer-video-1.0 || ! command -v cmake >/dev/null; then
  echo "Installing build dependencies..."
  apt-get update
  apt-get install -y cmake g++ pkg-config libopencv-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
fi

cmake -S . -B build -DFACESHIELD_WITH_VITIS="$VITIS"
cmake --build build -j"$(nproc)"
(cd build && ctest --output-on-failure)
echo "built: $(pwd)/build/faceshield_kv260"
