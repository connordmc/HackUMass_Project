# FaceShield laptop prototype

This is the local CPU face-blurring prototype. The existing README describes
separate hardware plans; no FPGA integration, networking, or recording is
implemented here. Python 3.9+ is required.

## Windows / VS Code

Open this project folder in VS Code and open a PowerShell terminal. If `py` is
unavailable, install Python with its launcher/PATH option, then reopen VS Code,
or replace `py` below with the full path to your Python executable.

```powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m faceshield
```

In VS Code, run **Python: Select Interpreter** from the command palette and
select `.venv\Scripts\python.exe`. Virtual environment activation is optional;
the explicit commands above also avoid PowerShell activation policy issues.

Focus the video window and press **q** to quit. Closing the window also exits.
If capture fails, close Teams/Zoom/other camera apps and enable camera access
for desktop apps in Windows Settings. Try another camera with:

```powershell
.\.venv\Scripts\python.exe -m faceshield --camera 1 --margin 0.20
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

## Linux

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m faceshield
.venv/bin/python -m unittest discover -s tests -v
```

A graphical desktop and GUI-capable OpenCV build are needed for `imshow`.
Do not use `opencv-python-headless` for this windowed prototype. Installing on
the KV260 will depend on its OS and available OpenCV packages; this has not
been verified on the board.

## Code and data flow

- `faceshield/__main__.py`: `VideoCapture` reads BGR images; the loop detects,
  scrubs the frame in place, and passes only that to `imshow`. `waitKey` handles GUI
  events and the quit key. Camera and windows are released in `finally`.
- `faceshield/detectors.py`: the bundled Haar cascade detects frontal faces
  on grayscale images. `detectMultiScale` searches multiple face sizes;
  `minNeighbors` filters weak candidates. Output is pixel `(x, y, width, height)`.
- `faceshield/scrubber.py`: corrupts regions in place and applies the
  fail-closed and staleness policy.
- `faceshield/blur.py`: independent of detection. Adds a configurable margin,
  clips boxes, reduces each region to at most 8x8 samples, Gaussian-smooths it,
  and resizes it back. Detail reduction keeps large faces strongly obscured
  without costly full-size Gaussian kernels. The original frame is preserved.
- `tests/test_blur.py`: deterministic synthetic images, using standard-library
  `unittest` and NumPy (installed as an OpenCV dependency).

A future detector only needs `detect(frame)` returning pixel boxes for that
same frame. Normalize coordinates, undo model resize/letterboxing, and convert
corner coordinates in a separate adapter once the actual DPU output is known.
There is deliberately no speculative FPGA API here.

## Webcam acceptance check

1. Launch the app; confirm a live window appears and your face is strongly blurred.
2. Move nearer/farther and across the frame; check that the blurred region follows.
3. Add a second person; check both faces, including near the image edges.
4. Assess smoothness at the requested 640x480 resolution (the camera may choose
   another size). Actual FPS and blur quality need checking on your laptop.
5. Press `q`, relaunch, and close the window; verify both release the webcam.

## Privacy policy implemented

- **In-place corruption:** `faceshield/scrubber.py` overwrites detected face
  regions (plus margin) with zeros in the captured frame itself. No unmasked
  copy is made or kept by this code, and only that frame is displayed.
- **Multiple faces:** every detected face is masked. Each box is tracked
  separately and kept for `--max-age`, so a face the detector drops for a
  frame stays masked even while other faces are still detected.
- **Fail-closed:** the whole frame is blanked if there is no detection yet,
  no valid face box, or no face has been seen within `--max-age` seconds
  (default 0.25). Consequence: with nobody in view the window is black, since
  a missed face cannot be told apart from an empty scene.
- Detector exceptions terminate the loop; no raw fallback is shown.
- `blur_faces` in `blur.py` is the older copy-and-blur helper and is no longer
  used by the app.

## What this does NOT guarantee

This is CPU software, not the hardware isolation in the README. The camera
driver, OS and any other process with the right privileges can still see the
raw frame before it is scrubbed. Haar can miss profiles, fast motion, small
faces, and poor lighting; each face box stays masked for `--max-age` after
its last detection, so a face missed for longer than that is shown. "Never accessible" requires moving the scrubber
into the PL, so the raw stream never reaches Linux-visible memory. That has
not been built or tested on the KV260.

## KV260 (Ubuntu 22.04) setup

The full step-by-step guide, including the per-boot camera script and
troubleshooting, is in [README.md → Board Setup (KV260)](README.md#board-setup-kv260).

Verified on Kria Ubuntu 22.04.4 (Python 3.10): unit tests pass and
detect + scrub runs at ~78 ms per 640x480 frame (~13 FPS) on the Cortex-A53.
Not yet tested with a live camera on the board.

**Network (direct cable, no internet on the board):**
- Board: `sudo nmcli con add type ethernet ifname eth0 con-name direct ipv4.method manual ipv4.addresses 192.168.10.2/24 ipv6.method link-local autoconnect yes && sudo nmcli con up direct`
- Mac: set the USB LAN adapter's IPv4 to Manual, `192.168.10.1` / `255.255.255.0`, no router.
- `ssh-copy-id ubuntu@192.168.10.2`

**Offline install** (the board has no `python3-venv`/`ensurepip`, so use the
system pip inside a pip-less venv):

```sh
# Mac: download aarch64 wheels for Python 3.10
pip download -d kv260-wheels --only-binary=:all: --platform manylinux2014_aarch64 \
  --python-version 3.10 --implementation cp -r requirements.txt
rsync -a --exclude .git --exclude .venv ./ ubuntu@192.168.10.2:~/HackUMass_Project/
rsync -a kv260-wheels/ ubuntu@192.168.10.2:~/kv260-wheels/

# Board
cd ~/HackUMass_Project
python3 -m venv --without-pip .venv
.venv/bin/python /usr/lib/python3/dist-packages/pip install --no-index \
  --find-links ~/kv260-wheels -r requirements.txt
.venv/bin/python -m unittest discover -s tests -v
```

**Board-specific gotchas:**
- Kria Ubuntu registers AMD's XRT as an OpenCL driver
  (`/etc/OpenCL/vendors/xilinx.icd`), and OpenCV's `detectMultiScale`
  segfaults inside it. `HaarFaceDetector` disables OpenCV's OpenCL use.
- Full-resolution Haar takes ~740 ms per frame on the A53, which would blank
  every frame. Detection runs on a 320-px-wide copy by default
  (`--detect-width`); boxes are mapped back to full size.
- The live app needs a camera (`/dev/video*`) and a display for `imshow`.
  A USB webcam works directly; the MIPI camera needs the Smart Camera
  firmware (`sudo xmutil loadapp kv260-smartcam`).

## Streaming to the laptop

`faceshield/stream.py` runs headless on the board: capture → detect → scrub
in place → JPEG → MJPEG over HTTP. Only the scrubbed JPEG bytes reach the
network side; on any capture or detector error the stream stops instead of
falling back.

```sh
# Board
.venv/bin/python -m faceshield.stream --test-pattern          # no camera
.venv/bin/python -m faceshield.stream --camera /dev/video0    # V4L2 / USB
.venv/bin/python -m faceshield.stream --gst "v4l2src device=/dev/video0"
```

On the laptop open `http://192.168.10.2:8080/` (live view plus fps/blanked
counters). Other endpoints: `/stream` (MJPEG), `/frame.jpg` (one frame),
`/stats` (JSON). Verified board → Mac over the direct cable at ~11 FPS with
the test pattern. The transport is unencrypted HTTP; it only ever carries
scrubbed frames, but anyone on the link can watch them.

## KV260 C++ build

The faster on-board version (IAS camera → DPU face detection → fail-closed
black fill → hardware H.264 → laptop, ~27 fps at 1080p) lives in
[`kv260/`](kv260/README.md), with its own setup and container guide. It
applies the same policy as this prototype: faces filled, 250 ms hold, whole
frame black when no face is current.
