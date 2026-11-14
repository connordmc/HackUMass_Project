# FaceShield on the Kria KV260

Everything runs on the board: the AR1335 IAS camera feeds the KV260, faces are
found on the DPU, overwritten with black on the board, compressed by the
hardware video encoder, and only the scrubbed H.264 stream goes over Ethernet.
The laptop just decodes and displays it.

It is **fail-closed**: a frame is only sent with a picture in it if the
detector currently knows where every face is. If no face is current (nobody
in view, a missed detection older than 250 ms, or a detector error), the whole
frame is black.

```
 AR1335 IAS camera (13 MP, 1/3.2")
        │ MIPI CSI-2
        ▼
 AP1302 ISP + MIPI RX (Smart Camera firmware, PL)        ┐
        │ NV12 1920x1080 @ 30, newest frame only         │
        ▼                                                │
 copy to a private buffer ──► 640x360 BGR thumbnail      │
        │                          │                     │  all on the
        │                          ▼                     │  KV260
        │                 densebox_640_360 on DPU        │
        │                          │ face boxes          │
        │                 hold boxes 250 ms, +25 %       │
        ▼                          │                     │
 fill faces black in NV12 ◄────────┘                     │
 (no current face or detector error                      │
  → whole frame black)                                   │
        │                                                │
        ▼                                                │
 VCU hardware H.264 (omxh264enc)                         │
        │                                                ┘
        ▼  Ethernet: MPEG-TS over TCP :5000 (or RTP/UDP)
 Laptop: viewer.py / ffplay  (never receives an unmasked frame)
```

## Detector: why AMD's densebox model instead of YOLO

The app uses `densebox_640_360`, the face detector that ships with AMD's Smart
Camera app, rather than the YOLO model in the top-level README:

- **It is trained for faces.** The README's YOLO-nano plan detects *cards*.
  A face YOLO would mean finding a face-trained YOLOv8n, quantizing it with
  Vitis AI, compiling it for this exact DPU, and writing YOLO post-processing.
  That's several hours of hackathon time with real risk.
- **It is already compiled for the DPU in the Smart Camera bitstream**, so it
  runs at camera frame rate with no toolchain work.
- It finds several faces per frame, and `--threshold` trades misses for false
  alarms. For privacy, err low: a false alarm blacks out a bit of background,
  but a miss on a frame where another face was found exposes that face.

The detector sits behind a small interface (`src/detector.hpp`), so a YOLO
xmodel can be added later without touching capture, censoring or streaming.

## Setup on the board

You need the KV260 running Kria Ubuntu 22.04, the AR1335 module plugged into
the IAS connector as in AMD's Smart Camera setup guide, and Ethernet to the
same network as the laptop.

```sh
git clone <this repo> && cd HackUMass_Project
git checkout claude/nice-galileo-lp3znj
kv260/scripts/start_container.sh     # loads smartcam firmware, opens container
# --- inside the container (/workspace/kv260) ---
scripts/check_board.sh               # camera, ISP, DPU, encoder, model, headers
scripts/build.sh                     # builds and runs unit tests
build/faceshield_kv260 --overlay     # streams on tcp port 5000
```

Every 5 seconds the app prints fps, DPU time and faces per frame. Stop it
with Ctrl+C.

## Watching on the laptop

```sh
python kv260/laptop/viewer.py --host <kv260-ip>
# or, without Python:
ffplay -fflags nobuffer -flags low_delay -framedrop tcp://<kv260-ip>:5000
```

`viewer.py` reconnects if the board restarts. For UDP (lower latency, but you
must name the laptop's address):

```sh
build/faceshield_kv260 --transport udp --host <laptop-ip>           # board
python kv260/laptop/viewer.py --sdp kv260/laptop/stream.sdp         # laptop
```

On Windows, allow Python/ffplay through the firewall for UDP.

## Options

| Option | Default | Notes |
|---|---|---|
| `--width/--height/--fps` | 1920 1080 30 | The Smart Camera ISP also supports 3840x2160 |
| `--detector` | `vitis` | `full-frame` blacks out everything: test the camera/stream path without the DPU |
| `--model` | Smart Camera's densebox_640_360 | Path to an `.xmodel` or a model name |
| `--threshold` | 0.7 | Lower catches more faces (AMD's own default is 0.9) |
| `--margin` | 0.25 | Extra fill around each face, as a fraction of its size |
| `--max-age` | 250 | ms a face stays filled after the detector last saw it; no current face = black frame |
| `--scrub` | `cpu` | `pl` = hardware scrubber (needs the kv260-faceshield firmware, see [pl/README.md](pl/README.md)) |
| `--raw-source` | | `pl` only: GStreamer source for the raw detector stream |
| `--uio` | `/dev/uio4` | `pl` only: scrubber register device |
| `--transport` | `tcp` | `udp` needs `--host` |
| `--encoder` | `omx` | VCU hardware. `x264` = software (slow at 1080p on the A53s) |
| `--bitrate` | 8000 | kbit/s |
| `--source` | `mipi` | Or any GStreamer source, e.g. `"v4l2src device=/dev/video2"` for a USB webcam |
| `--overlay` | off | Draws fps and face count into the stream |
| `--test-no-blank` | off | **Test only, not fail-closed:** frames with no detected face are sent unmasked instead of black (detected faces are still filled). A "TEST MODE: NOT FAIL-CLOSED" bar is burned into every frame. `cpu` scrub only |

## Privacy behaviour

Same policy as the Python prototype (`faceshield/scrubber.py`):

- **Faces are destroyed, not blurred.** Every pixel in a face box (plus
  `--margin`) is overwritten with black (Y = 16, UV = 128) in both planes.
- **Fail closed on no face.** If no box is current, the whole frame is black.
  An empty scene and a missed face look the same to a detector, so both
  are blanked.
- **Time-based hold.** A face stays filled for `--max-age` (250 ms) after the
  detector last saw it, so a brief miss neither exposes it nor flickers to
  black. Each face is tracked separately: if one of two faces is missed, both
  stay covered. A slow frame rate can't stretch the hold, because it's
  measured in time, not frames.
- **Fail closed on errors.** If detection throws, that frame goes out black;
  after 30 failures in a row the app exits.
- Only scrubbed buffers are ever handed to the encoder. There is one push
  point in `main.cpp`, after scrubbing.
- Capture keeps only the newest camera frame, so a slow frame causes a
  dropped frame, never a growing delay.
- `--scrub pl` moves the filling into the PL scrubber, so the clean buffer
  is masked in hardware before Linux sees it (see [pl/README.md](pl/README.md)).

Limits, stated plainly:

- A face the detector never finds (strong profile, very small, motion blur,
  bad light) is only covered if the frame is blanked. If the detector finds
  another face in the same frame, the missed face goes out unmasked.
- With `--scrub cpu`, raw frames exist in the board's ARM memory before scrubbing. This build meets
  "nothing unmasked leaves the board". It does not meet the top-level README's
  stronger goal of "no software on the board can read raw pixels", which needs
  the PL scrubber / on-chip detector described there.
- The stream is not encrypted. Use a direct cable or a trusted network.

## What has and hasn't been verified

Fail-closed rework, checked on macOS with OpenCV 5.1 (the board uses OpenCV 4):
- `tests/test_censor.cpp` passes under AddressSanitizer and UBSan. It covers:
  - Fill inside the box only, including row padding and chroma.
  - The whole frame goes black when there are no valid boxes.
  - The time-based hold: max age, a slow frame rate, and future timestamps.
  - Multi-face partial misses.
  - The PL driver's commit handshake, box merging and bounds check, against a fake register bank.
- `pl/scrubber/run_csim.sh` passes (13 cases).
- Mutation checks: deliberately breaking each fail-closed rule makes the tests fail.
- `main.cpp` was only type-checked against stub GStreamer declarations. It
  hasn't been compiled against real GStreamer or run since the rework. Build
  it in the Smart Camera container (`scripts/build.sh`) before relying on it.

Earlier build (blur version):

Verified here, on an x86 machine without the board:
- Unit tests: censoring changes only the face boxes, including row padding and
  chroma; malformed boxes are ignored; box hold; black fill; pipeline strings.
- End to end with a test video source and a software H.264 encoder: the app
  streamed over TCP and RTP/UDP, and OpenCV (as `viewer.py` uses it) decoded
  1280x720 at about 25–28 fps. With `--detector full-frame`, random noise with
  luma stddev 63 arrived with stddev under 1 (measured with the earlier blur build; the fill is a constant, so it survives encoding trivially).
- The Vitis AI detector code compiles against the `FaceDetect` API
  (`create`, `setThreshold`, `run`, normalized `rects`).

**Not yet run on a KV260.** Check these first on the board:
- `mediasrcbin` capture of the AR1335 at 1080p NV12 with `io-mode=mmap`.
  If it fails, try the exact source string the Smart Camera app prints.
- `omxh264enc` property names on your image's version. `--encoder` takes a
  replacement string if one is rejected.
- That the Smart Camera container has the Vitis AI Library *headers*
  (`check_board.sh` reports this). If not, install the Vitis AI Library dev
  package that matches the container's Vitis AI version (2.5 for smartcam
  2022.1).
- Real frame rate and DPU time. Expect DPU inference to be a few milliseconds;
  the CPU copy and thumbnail are the main per-frame costs at 1080p. If needed,
  run at 1280x720 or move the thumbnail step into the PL.

## Files

```
kv260/
  src/main.cpp         capture loop, fail-closed handling, stats
  src/censor.*         NV12 fill, fail-closed scrub, box expansion, thumbnail, time-based hold
  src/pl_scrubber.*    driver for the PL scrubber IP (--scrub pl)
  pl/                  PL scrubber IP (HLS), testbench, Vivado integration
  src/detector.*       Vitis AI densebox detector + full-frame bring-up detector
  src/pipeline.*       GStreamer capture / encode / transport strings
  tests/test_censor.cpp
  scripts/start_container.sh   load firmware, start Smart Camera container
  scripts/check_board.sh       readiness checks on the board
  scripts/build.sh             build + unit tests
  laptop/viewer.py, stream.sdp viewing on the laptop
```
