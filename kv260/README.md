# FaceShield on the Kria KV260

Everything runs on the board: the AR1335 IAS camera feeds the KV260, faces are
found on the DPU, blurred on the board, compressed by the hardware video
encoder, and only the censored H.264 stream goes over Ethernet. The laptop just
decodes and displays it.

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
        │                 hold boxes ~5 frames, +25 %    │
        ▼                          │                     │
 blur faces in NV12 ◄──────────────┘                     │
 (detector error → whole frame black)                    │
        │                                                │
        ▼                                                │
 VCU hardware H.264 (omxh264enc)                         │
        │                                                ┘
        ▼  Ethernet: MPEG-TS over TCP :5000 (or RTP/UDP)
 Laptop: viewer.py / ffplay  (never receives an unblurred frame)
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
  alarms. For privacy, err low: a false alarm blurs a bit of background, but a
  miss exposes a face.

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
| `--detector` | `vitis` | `full-frame` blurs everything: test the camera/stream path without the DPU |
| `--model` | Smart Camera's densebox_640_360 | Path to an `.xmodel` or a model name |
| `--threshold` | 0.7 | Lower catches more faces (AMD's own default is 0.9) |
| `--margin` | 0.25 | Extra blur around each face, as a fraction of its size |
| `--hold` | 5 | Frames a face stays blurred after the detector loses it |
| `--blocks` | 8 | Faces are reduced to N×N samples; lower = stronger |
| `--transport` | `tcp` | `udp` needs `--host` |
| `--encoder` | `omx` | VCU hardware. `x264` = software (slow at 1080p on the A53s) |
| `--bitrate` | 8000 | kbit/s |
| `--source` | `mipi` | Or any GStreamer source, e.g. `"v4l2src device=/dev/video2"` for a USB webcam |
| `--overlay` | off | Draws fps and face count into the stream |

## Privacy behaviour

- Only `censor`-processed buffers are ever handed to the encoder. There is one
  push point in `main.cpp`, after blurring.
- **Fail closed on errors:** if detection throws, that frame goes out black;
  after 30 failures in a row the app exits.
- **Boxes are held** for `--hold` frames and padded by `--margin`, so a single
  missed detection or quick movement doesn't flash a face.
- Each frame is blurred with boxes detected on *that same frame* (no
  one-frame lag), because the DPU is fast enough to run inline.
- Capture keeps only the newest camera frame, so a slow frame causes a dropped
  frame, never a growing delay. This avoids the backlog lag seen in the laptop
  prototype.

Limits, stated plainly:

- A face the detector never finds (strong profile, very small, motion blur,
  bad light) is not blurred. Blur reduces identifiability; it isn't a guarantee.
- Raw frames exist in the board's ARM memory (CPU-side blur). This build meets
  "nothing unblurred leaves the board". It does not meet the top-level README's
  stronger goal of "no software on the board can read raw pixels", which needs
  the PL scrubber / on-chip detector described there.
- The stream is not encrypted. Use a direct cable or a trusted network.

## What has and hasn't been verified

Verified here, on an x86 machine without the board:
- Unit tests: censoring changes only the face boxes, including row padding and
  chroma; malformed boxes are ignored; box hold; black fill; pipeline strings.
- End to end with a test video source and a software H.264 encoder: the app
  streamed over TCP and RTP/UDP, and OpenCV (as `viewer.py` uses it) decoded
  1280x720 at about 25–28 fps. With `--detector full-frame`, random noise with
  luma stddev 63 arrived with stddev under 1, so the blur survives encoding.
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
  src/censor.*         NV12 blur, box expansion, black fill, thumbnail, box hold
  src/detector.*       Vitis AI densebox detector + full-frame bring-up detector
  src/pipeline.*       GStreamer capture / encode / transport strings
  tests/test_censor.cpp
  scripts/start_container.sh   load firmware, start Smart Camera container
  scripts/check_board.sh       readiness checks on the board
  scripts/build.sh             build + unit tests
  laptop/viewer.py, stream.sdp viewing on the laptop
```
