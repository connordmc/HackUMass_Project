#!/bin/bash
# Run on the KV260 (Kria Ubuntu 22.04) host. Loads the Smart Camera firmware
# (MIPI/ISP pipeline for the AR1335 IAS camera + DPU + VCU) and opens a shell in
# AMD's Smart Camera container with this repository mounted at /workspace.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=${SMARTCAM_IMAGE:-xilinx/smartcam:2022.1}

if ! xmutil listapps | grep -q kv260-smartcam; then
  echo "Installing the Smart Camera firmware package..."
  sudo apt-get install -y xlnx-firmware-kv260-smartcam
fi
sudo xmutil unloadapp || true
sudo xmutil loadapp kv260-smartcam

docker image inspect "$IMAGE" >/dev/null 2>&1 || docker pull "$IMAGE"

exec docker run -it --rm --privileged --net=host -h xlnx-docker \
  -v /tmp:/tmp -v /dev:/dev -v /sys:/sys -v /run:/run \
  -v /etc/vart.conf:/etc/vart.conf \
  -v /lib/firmware/xilinx:/lib/firmware/xilinx \
  -v "$REPO":/workspace -w /workspace/kv260 \
  "$IMAGE" bash
