#!/bin/bash
# Run on the KV260 (Kria Ubuntu 22.04) host. Loads the Smart Camera firmware
# (MIPI/ISP pipeline for the AR1335 IAS camera + DPU + VCU) and opens a shell in
# AMD's Smart Camera container with this repository mounted at /workspace.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
# faceshield-smartcam is the smartcam image with the build dependencies already
# installed (made with: docker commit <container> faceshield-smartcam:2022.1),
# so starting the container needs no downloads. Fall back to AMD's image.
if [ -z "${SMARTCAM_IMAGE:-}" ] && docker image inspect faceshield-smartcam:2022.1 >/dev/null 2>&1; then
  IMAGE=faceshield-smartcam:2022.1
else
  IMAGE=${SMARTCAM_IMAGE:-xilinx/smartcam:2022.1}
fi

# xmutil needs root, so ask dpkg whether the firmware package is installed.
if ! dpkg -s xlnx-firmware-kv260-smartcam >/dev/null 2>&1; then
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
