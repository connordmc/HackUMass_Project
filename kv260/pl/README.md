# FaceShield PL scrubber

A hardware block on the camera's AXI4-Stream path that overwrites face
pixels, and blanks whole frames that have no current face, **before the clean
frame is written to memory**. Linux and the network app only ever see the
clean frame buffer. There is no software masking step that could be skipped.

```
 AR1335 ─MIPI─► AP1302 ─MIPI─► CSI-2 RX ─► tee ─┬─► faceshield_scrubber ─► frmbuf_wr ─► clean DDR ─► encoder ─► network
                                                └──────────────────────► frmbuf_wr ─► raw DDR ─► DPU detector
                                                                                                    │ boxes
                                     faceshield_scrubber ◄── AXI-Lite (boxes + commit) ── faceshield_kv260 --scrub pl
```

## Status

| Part | State |
|---|---|
| Scrubber logic (`scrubber/scrubber_core.hpp`) | Written, testbench passes on the host compiler (13 tests, sanitizers on) |
| HLS top function (`scrubber/scrubber.cpp`), `run_hls.tcl` | Written, **not yet synthesized** (needs Vitis HLS 2022.1 on Linux x86) |
| Block-design insertion (`integration/insert_scrubber.tcl`) | Written, **untested** |
| Device-tree overlay (`integration/faceshield-overlay.dtsi`) | Sketch, **untested** |
| App driver (`src/pl_scrubber.*`, `--scrub pl`) | Written, unit-tested against a fake register bank; never run on hardware |

Until a bitstream is built, use the default `--scrub cpu`, which applies the
same policy on the ARM cores.

## Policy enforced in hardware

- **Reset state is blank.** Until software commits boxes, every pixel is filled.
- **Each box is filled** with a constant (Y = 16, C = 128, i.e. black) as
  pixels stream through. Nothing is buffered, so nothing can be read back.
- **No face, no picture.** A commit with zero boxes blanks the whole frame.
- **Staleness.** If software has not committed for `max_age_frames` frames
  (default 8, about 250 ms at 30 fps), the frame is blanked. If the app
  crashes or hangs, the output goes black on its own.
- **Malformed input blanks.** Inverted or out-of-frame boxes, `num_boxes > 8`,
  or zero width/height blank the frame. So do pixels beyond the configured
  line length or frame height.
- **No tearing.** Boxes are copied at a start of frame, and only when
  `commit_seq` changes. The driver writes nothing until the hardware reports
  the previous commit latched (`latched_seq`), so a frame can't mix two
  commits.

The detector runs on the raw branch, so boxes are applied to a frame or two
after the one they were found in. `--margin` (default 25 %) covers normal
head movement in that time.

## Registers (AXI-Lite, bundle `ctrl`)

| Register | Dir | Meaning |
|---|---|---|
| `width`, `height` | W | Frame size in pixels |
| `fill_y`, `fill_c` | W | Fill values (16, 128) |
| `max_age_frames` | W | Frames a commit stays valid |
| `num_boxes` | W | 0–8 |
| `box<b>_x`, `box<b>_y` | W | `x0 \| x1 << 16`, `y0 \| y1 << 16`, half-open |
| `commit_seq` | W | Change it (after writing the boxes) to commit |
| `frames`, `blanked_frames` | R | Counters |
| `frames_since_commit` | R | Age of the active commit |
| `latched_seq` | R | Last commit the hardware copied |

HLS assigns the offsets. `src/pl_scrubber_regs.hpp` holds predicted values,
and `--scrub pl` refuses to run until it is regenerated from the export (step 2 below).

## Building (needs Linux x86 with Vivado/Vitis 2022.1)

1. **Confirm the stream format first.** Open the smartcam block design and
   check the CSI-2 RX `video_out` width and pixel packing:
   - The scrubber assumes 2 bytes per pixel: byte 0 = Y, byte 1 = C (UG934 YUV 4:2:0).
   - `insert_scrubber.tcl` prints the bytes per beat. `PPC = bytes / 2`.
   - If the packing differs, adjust the fill in `scrubber_core.hpp`. A plain
     `fill_y = fill_c = 0` still destroys the face whatever the packing; it
     only tints the masked area green.
2. **Build the IP:**
   ```sh
   cd kv260/pl/scrubber
   ./run_csim.sh                                    # host testbench
   FACESHIELD_PPC=<ppc> vitis_hls -f run_hls.tcl    # csim + synthesis + IP export
   python3 gen_regs.py faceshield_scrubber_prj/solution1/impl/ip/drivers/*/src/xfaceshield_scrubber_hw.h \
     > ../../src/pl_scrubber_regs.hpp
   ```
   Check the synthesis report for II = 1 at the video clock.
3. **Insert it into the platform.**
   - Clone `Xilinx/kria-vitis-platforms` at the 2022.1 tag.
   - Open the kv260 smartcam platform project in Vivado and add `faceshield_scrubber_prj/solution1/impl/ip` to its IP repository.
   - Open the block design and run `source kv260/pl/integration/insert_scrubber.tcl`.
   - Do the TODOs it prints, then validate.
4. **Rebuild the platform and the DPU overlay** with the repository's
   makefiles: platform first, then the smartcam overlay that links the DPU.
   This produces the `.bit.bin` and `.xclbin`.
5. **Device tree.** Generate the overlay from the new `.xsa` (`xsct` /
   `createdts`) and merge in `integration/faceshield-overlay.dtsi`, using the
   real addresses and labels.
6. **Install on the board** as a new app, next to the stock one:
   ```sh
   sudo mkdir -p /lib/firmware/xilinx/kv260-faceshield
   sudo cp kv260-faceshield.bit.bin kv260-faceshield.dtbo kv260-faceshield.xclbin shell.json \
     /lib/firmware/xilinx/kv260-faceshield/
   sudo xmutil unloadapp; sudo xmutil loadapp kv260-faceshield
   ```
7. **Run:**
   ```sh
   build/faceshield_kv260 --scrub pl --uio /dev/uioN \
     --source mipi \
     --raw-source "v4l2src device=/dev/video1 io-mode=mmap"
   ```
   The app checks that the scrubber's frame counter keeps advancing. If the
   clean stream isn't passing through the scrubber, it stops.

## Verification on hardware

- After `loadapp`, before starting the app: capture from the clean device. Every frame must be black, because the reset state is blank.
- Kill the app mid-stream: the output must go black within about 250 ms.
- With a face in view: the face is black, the background visible.
- With no one in view: the frame is black.
- `blanked_frames` and `frames` in the app's 5-second stats line must match what you see.

## What this does and doesn't guarantee

- **Guaranteed by hardware** once deployed: the clean buffer never contains
  unmasked face pixels, unless the detector gives a wrong box or misses a
  face. No software bug or crash can expose a frame: without fresh commits,
  frames go black.
- **Not guaranteed:**
  - Raw frames still reach DDR for the DPU, in the raw buffer, and root or
    kernel code can read that memory. This matches the hackathon design in
    the top-level README.
  - Closing that gap needs the on-chip detector (FINN / hls4ml / HLS
    classical detector) or an XMPU firewall around the raw buffer, both
    described in the top-level README.
  - A face the detector never finds is not masked on a frame where another
    face *was* found, because only an empty commit blanks the frame.
