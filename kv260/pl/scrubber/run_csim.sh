#!/bin/bash
# Runs the scrubber testbench with the host compiler (no Vitis HLS needed).
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)/tb_scrubber
${CXX:-c++} -std=c++17 -Wall -Wextra -Wno-unknown-pragmas -O1 \
  -fsanitize=address,undefined tb_scrubber.cpp -o "$out"
"$out"
