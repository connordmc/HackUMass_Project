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

Tested on Kria Ubuntu 22.04 with the AR1335 module on **J7 (IAS0)**. The
board is connected to the Mac by a direct Ethernet cable (board
`192.168.10.2`, Mac `192.168.10.1`); see the top-level README for the
network, SSH and camera-cable setup.

### One-time setup (needs internet on the board, ~1 GB)

The board has no Wi-Fi. If the Mac is on Wi-Fi (or an iPhone hotspot), it can
share its connection over the cable:

```sh
# Mac: forward the board's traffic out through Wi-Fi (en0)
route -n get default | grep interface        # use this interface below if not en0
sudo sysctl -w net.inet.ip.forwarding=1
echo "nat on en0 from 192.168.10.0/24 to any -> (en0)" | sudo pfctl -a com.apple/faceshield -f -
sudo pfctl -E                                 # the ALTQ / "-f" warnings are normal

# Board: use the Mac as router and set DNS, then check
sudo nmcli con mod direct ipv4.gateway 192.168.10.1 ipv4.dns "1.1.1.1 8.8.8.8"
sudo nmcli con up direct                      # SSH may freeze for a few seconds
ping -c 3 google.com
```

Then install Docker and give your user access to it:

```sh
sudo apt update                               # the final "apt-check" step is slow, wait for the prompt
sudo apt install -y docker.io
sudo usermod -aG docker ubuntu
exit                                          # log out and SSH back in for the group to apply
docker run --rm hello-world
```

Copy the code to the board from the Mac (or `git clone` on the board):

```sh
# Mac, repository root, on this branch
rsync -a --exclude .git --exclude .venv ./ ubuntu@192.168.10.2:~/HackUMass_Project/
```

Open the container and build (first time downloads the 0.56 GB Smart Camera
image plus compilers and libraries):

```sh
cd ~/HackUMass_Project
kv260/scripts/start_container.sh
# now at root@xlnx-docker:/workspace/kv260#
scripts/check_board.sh
scripts/build.sh                              # builds and runs the unit tests
```

Save the container with everything installed, so later starts need no
downloads (run on the board, in a second SSH session, while the container is
open):

```sh
docker commit $(docker ps -q | head -1) faceshield-smartcam:2022.1
```

`start_container.sh` uses `faceshield-smartcam:2022.1` automatically when it
exists. After this the board needs no internet; undo the sharing on the Mac:

```sh
sudo pfctl -a com.apple/faceshield -F all
sudo sysctl -w net.inet.ip.forwarding=0
```

### Getting into the container (`root@xlnx-docker:/workspace/kv260#`)

Every time, after SSHing into the board:

```sh
ssh ubuntu@192.168.10.2
cd ~/HackUMass_Project
kv260/scripts/start_container.sh
```

It loads the Smart Camera firmware (camera, DPU, encoder; asks for your sudo
password), starts the container and leaves you at
`root@xlnx-docker:/workspace/kv260#`. `~/HackUMass_Project` on the board is
`/workspace` inside, so the build in `kv260/build/` survives restarts.
`exit` leaves (and removes) the container.

To open a **second shell in the same running container** (e.g. while the app
runs in the first):

```sh
docker exec -it $(docker ps -q | head -1) bash -c "cd /workspace/kv260 && exec bash"
```

Only one program can use the camera: stop the Python stream
(`faceshield.stream`) before running the C++ app, and vice versa.

### Running it

Inside the container. Wait about a minute after starting: loading the face
model onto the DPU takes that long before the first frame (a viewer that
connects earlier just sees nothing yet).

```sh
# Fail-closed demo, UDP straight to the Mac (lowest latency)
build/faceshield_kv260 --overlay --transport udp --host 192.168.10.1 --bitrate 4000

# Same over TCP (any number of viewers can connect at any time)
build/faceshield_kv260 --overlay

# Test mode: show frames with no detected face instead of black
build/faceshield_kv260 --overlay --test-no-blank --transport udp --host 192.168.10.1 --bitrate 4000
```

Every 5 seconds the app prints fps, DPU time, faces per frame and how many
frames were blanked. `Ctrl+C` stops it. The `GStreamer-CRITICAL ...
IS_MUTABLE` lines at start and "The specified Level is too low" come from
AMD's camera and encoder plugins and are harmless.

## Watching on the laptop

Install a player once: `brew install mpv ffmpeg`.

**UDP** (`--transport udp`). Create the stream description once (RTP doesn't
describe itself), then play it:

```sh
printf 'v=0\no=- 0 0 IN IP4 0.0.0.0\ns=FaceShield KV260\nc=IN IP4 0.0.0.0\nt=0 0\nm=video 5000 RTP/AVP 96\na=rtpmap:96 H264/90000\n' > ~/faceshield.sdp

mpv --profile=low-latency --untimed --no-cache '--demuxer-lavf-o=protocol_whitelist=[file,udp,rtp]' ~/faceshield.sdp
# or
ffplay -protocol_whitelist file,udp,rtp -fflags nobuffer -flags low_delay -framedrop -reorder_queue_size 0 ~/faceshield.sdp
```

(The same file is in the repo as `kv260/laptop/stream.sdp`.) Keep the quotes
around the `--demuxer-lavf-o=...` option: without them zsh treats the
brackets as a filename pattern ("no matches found"). If macOS asks whether
mpv/ffplay may accept incoming connections, allow it, or the packets are
blocked. Gray smears or blocks mean lost packets: lower `--bitrate`.

**TCP** (default transport):

```sh
mpv --profile=low-latency --untimed --no-cache tcp://192.168.10.2:5000
# or
ffplay -probesize 32 -analyzeduration 0 -fflags nobuffer -flags low_delay -framedrop tcp://192.168.10.2:5000
```

Plain `ffplay tcp://...` without those flags reads several MB before playing
and then stays that far behind. `python kv260/laptop/viewer.py --host
192.168.10.2` (or `--sdp kv260/laptop/stream.sdp` for UDP) also works and
reconnects if the board restarts, but buffers more than mpv.

The encoder sends a keyframe with its parameter sets every second, so a viewer
can join at any time; the picture appears within about a second of
connecting (after the start-up minute above).

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
| `--bitrate` | 8000 | kbit/s. For UDP, 2500–4000 avoids packet loss on big keyframes |
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

**On the KV260** (Kria Ubuntu 22.04, smartcam 2022.1 container, AR1335 on J7):
- Builds with no warnings; `ctest` passes on the board (`censor`, `pl_scrubber`).
- `mediasrcbin` delivers 1920x1080 NV12. The DPU densebox detector runs at
  ~21 ms per frame, and the whole loop at **27–28 fps at 1080p**
  (25–31 ms per frame).
- With no face in view every frame is blanked (fail-closed). With
  `--test-no-blank`, frames pass through with the banner (decoded frame
  checked on the Mac). `--scrub pl --test-no-blank` is rejected.
- TCP stream decodes on macOS (ffmpeg) when joining mid-stream, after adding
  periodic IDR frames to the encoder.

**Not yet verified:**
- Masking a live face on the board (the camera faced the ceiling in all runs
  so far).
- UDP end to end on the Mac, and the remaining viewing latency.
- `--scrub pl` (needs the PL scrubber bitstream; see [pl/README.md](pl/README.md)).

**Host checks** (macOS, OpenCV 5.1): `tests/test_censor.cpp` passes under
AddressSanitizer and UBSan, `pl/scrubber/run_csim.sh` passes (13 cases), and
deliberately breaking each fail-closed rule makes the tests fail.

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
