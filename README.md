# HackUMass Project

**Redaction of sensitive information at the camera, in hardware, before any pixel reaches the network.**

**Stage 1 (hackathon demo): faces. Stage 2: cards and other documents.**

Built on the AMD Kria KV260 for HackUMass.

---

## The Problem

When a camera feed is sent over a network, everything in the frame can be intercepted. If a face is in frame, anyone tapping the link sees it. If someone photographs a credit card and an attacker is tapping the link between the camera and the receiving machine, the card number is exposed. Encryption in transit helps, but the receiving host, its software, and its memory all still see the raw image.

## The Idea

Move the privacy boundary onto the edge device. Frames are captured and processed on the Kria KV260, and sensitive regions are masked **in the FPGA fabric** on the output path. Only sanitized frames are ever handed to the network stack. The unredacted image never leaves the board.

**Stage 1 demo (faces):** detect faces in the camera feed and **corrupt those pixels in place** on the board, so the original face data is destroyed before the image is sent to the laptop. If no valid, current face detection exists, the whole frame is blanked (fail-closed).

**Stage 2 (cards):** the same pipeline with a card detector (see [Stage 2](#stage-2-card-redaction)).

## Threat Model

| Guaranteed in the hackathon build | Planned after the hackathon | Out of scope |
|---|---|---|
| Passive or active interception of the Ethernet link | Root or kernel-level compromise of Linux on the Kria (see [Next Steps](#next-steps-after-the-hackathon)) | Physical attacks (probing, JTAG, bitstream extraction) |
| Compromised or buggy software on the receiving laptop | | Side-channel attacks on the FPGA |
| The network-facing application sending an unmasked frame (it only ever receives scrubbed buffers) | | A malicious bitstream |

**Hackathon guarantee (target, hardware build):** no unmasked face is ever transmitted off the board.

**What the code in this repo currently provides:** the `faceshield/` Python prototype implements the policy (in-place corruption, fail-closed blanking, stale-detection blanking) and is tested on synthetic frames on a laptop. It is software on the CPU: it does **not** isolate raw frames from the OS, camera driver or other software. The isolation claims above hold only once the scrubber is in the PL. See [FACESHIELD.md](FACESHIELD.md).

---

## System Architecture (Hackathon Build)

```
┌──────────────────────────────── Kria KV260 (Zynq UltraScale+ MPSoC) ────────────────────────────────┐
│                                                                                                      │
│  Programmable Logic (PL)                                                                             │
│  ┌────────┐ MIPI CSI-2 ┌───────────┐       AXI-Stream        ┌──────────────────┐                     │
│  │ Camera │───────────▶│ ISP/video │──────────┬─────────────▶│   Scrubber IP    │──────────┐          │
│  └────────┘            │ pipeline  │          │              │ (corrupt bbox,      │          │          │
│                        └───────────┘          │ tee          │  fail-closed)    │          │          │
│                                               ▼              └────────▲─────────┘          ▼          │
│                                       ┌──────────────┐                │ AXI-Lite:  ┌──────────────┐   │
│                                       │ Raw DMA write│                │ bbox+valid │ Clean DMA    │   │
│                                       └──────┬───────┘     ┌──────────┴───────┐    │ write        │   │
│                                              │             │ DPU: INT8        │    └──────┬───────┘   │
│                                              │             │ YOLO-nano        │           │           │
│                                              │             └──────────▲───────┘           │           │
│ ─────────────────────────────────────────────┼────────────────────────┼───────────────────┼─────────  │
│  DDR / Processing System (ARM Cortex-A53, PetaLinux)                  │                   │           │
│                                              ▼                        │                   ▼           │
│                            ┌──────────────────────────────┐           │     ┌─────────────────────┐   │
│                            │ Reserved DDR region          │───────────┘     │ Clean frame buffer  │   │
│                            │ (raw frames; DPU input only) │                 │ (/dev/video*)       │   │
│                            └──────────────────────────────┘                 └──────────┬──────────┘   │
│                                                                                        ▼              │
│                                                        ┌─────────────────────────────────────────┐   │
│                                                        │ Network app: read clean frame → JPEG →   │   │
│                                                        │ WebSocket (never touches raw region)     │   │
│                                                        └──────────────────────┬──────────────────┘   │
└───────────────────────────────────────────────────────────────────────────────┼──────────────────────┘
                                                                                │ Ethernet (sanitized only)
                                                                                ▼
                                                                  ┌──────────────────────────┐
                                                                  │ Laptop: browser client    │
                                                                  │ renders JPEG from socket  │
                                                                  └──────────────────────────┘
```

### 1. Starting point: the KV260 Smart Camera stack

We build on AMD's KV260 Smart Camera reference platform. It provides a working MIPI camera, ISP, DPU and PetaLinux pipeline out of the box, so the build time goes into the privacy pipeline instead of camera bring-up.

### 2. Detection: quantized YOLO-nano on the DPU

- Model: a face detector (e.g. YOLOv8n-face or YOLOv5n-face), quantized to INT8 with Vitis AI, running on the DPU (DPUCZDX8G).
- Input: raw frames DMA'd into a **reserved DDR region** (`reserved-memory` in the device tree). The region holds detector input only and is never exposed to the network application.
- Output: face bounding boxes `(x_min, y_min, x_max, y_max)` plus a valid flag, written to the scrubber's AXI-Lite registers.

### 3. Scrubbing: inline in programmable logic (AXI-Stream)

- A custom IP block (HLS or SystemVerilog) sits on the AXI-Stream path between the camera pipeline and the **clean** frame-buffer DMA.
- Pixels inside the current bounding boxes (plus a safety margin) are **overwritten with a constant fill** as they stream through. The original values are not kept anywhere on the clean path, so they cannot be recovered downstream.
- The output buffer that Linux and the network app read from has already been scrubbed in hardware. There is no software masking step that could be skipped or bypassed.

### 4. Timing and fail-closed behavior

- **Previous-frame boxes:** the DPU detects on frame N−1, and the scrubber applies that box to frame N (about 16 ms later at 60 FPS), padded to cover small motion.
- **Fail-closed by default:** the scrubber blanks or drops the **entire frame** unless a valid, current detection is present. For faces, "no face detected" is treated the same as a missed detection, so an empty scene is blanked too. A detection older than a max age (default 250 ms in the prototype; the last good box is reused until then to ride out detector dropouts) counts as stale. If detection is late, missing or stale, nothing unmasked goes out.
- **Snapshot mode for the demo:** hold your face steady, trigger capture, let the DPU update the box, and send the next scrubbed frame.

### 5. Transport: WebSocket and JPEG snapshots over Ethernet

1. The network app reads a sanitized frame from the clean buffer (V4L2 / OpenCV).
2. It encodes the frame with `cv2.imencode('.jpg', frame)`.
3. It pushes the JPEG bytes over a binary WebSocket (Python `websockets` or FastAPI).
4. The laptop's browser client renders each blob with `URL.createObjectURL(blob)`.

**Expected latency:** under ~30–50 ms per frame over local Ethernet.

---

## Hackathon Plan

### Phase 1: Baseline (must ship)
- [ ] Smart Camera platform running on the KV260 with a live MIPI feed
- [ ] INT8 YOLO-nano face detector on the DPU, reading from the reserved DDR region
- [ ] PL scrubber IP on the clean output path, box set over AXI-Lite
- [ ] Fail-closed gate: full-frame blank when there is no valid detection
- [ ] Python WebSocket server sending sanitized JPEG snapshots
- [ ] Browser client on the laptop displaying received frames

### Phase 2: Stretch (if time allows)
- [ ] Classical-vision face detector in HLS (e.g. Haar-style cascade) reading a downscaled copy of the frame held in on-chip BRAM. If this works, it replaces the DPU path and enables the full hardware-isolation claim during the demo.
- [ ] Continuous throttled streaming (5–10 FPS) instead of single snapshots
- [ ] Blur instead of solid fill

### Fallback
If the AXI-Stream scrubber integration stalls, prototype end to end with OpenCV masking on the ARM cores, then move the mask into the PL.

---

## Next Steps (After the Hackathon)

### Stage 2: card redaction

Swap the face detector for a card detector and keep everything else:
- YOLO-nano card class, or a classical HLS detector (color/edge threshold → largest rectangular blob → ISO card aspect-ratio check, ~1.586).
- Unlike faces, a card is expected to be held in view, so fail-closed on "no detection" is a natural fit.
- Fine-grained option: mask only the number band or digits.

### Take the raw frame out of reserved DDR so that kernel-level code can't reach it

In the hackathon build the raw frame sits in a reserved DDR region. That keeps it away from the network and the application, but a reserved region only hides memory from the kernel's allocator. Root or kernel-level code can still map it with `ioremap` or `/dev/mem`. The goal is for **no software on the board, even with kernel privileges, to be able to read an unmasked frame.**

**Primary approach: on-chip detection from BRAM**
- Tee the camera stream in the PL. Full resolution goes to the scrubber. A downscaled copy (e.g. 224×224 or 320×320) is written into ping-pong buffers in BRAM/URAM.
- The detector reads the buffer through native BRAM ports, or a PL-only AXI interconnect with no PS master attached. The buffer is never placed behind an AXI BRAM controller on the PS's ports, so Linux has no physical address that reaches it.
- The detector sends the box and valid flag to the scrubber over internal PL signals instead of PS-writable registers.
- The detector must keep its weights and **every intermediate activation** on-chip. That rules out the Vitis AI DPU, which keeps its input, weights and feature maps in DDR (early-layer feature maps are close to a reconstructable image). Candidates: a small quantized CNN built as a dataflow accelerator with **FINN** or **hls4ml**, or the Phase 2 classical-vision detector.
- Memory budget on the K26 SOM: ~648 KB of BRAM + ~2.3 MB of URAM. A 320×320 RGB ping-pong pair uses ~600 KB, and a 224×224 grayscale pair ~100 KB. The rest is available for a small single-class model.

**Alternative approach: firewall DDR with the XMPU**
- The ZynqMP's XMPU (DDR memory protection unit) can block the ARM cores from a DDR region while still allowing the DPU's AXI masters in. It is configured at boot and can be locked until reset.
- This keeps the DPU and YOLO, but requires Vitis AI's runtime (VART) to allocate the DPU's input and feature-map buffers inside the protected region. That needs custom allocation work.

**Hardening that goes with it**
- No ILA, debug cores or readback registers on raw-pixel paths in the production bitstream.
- Secure boot and bitstream encryption so that the isolation logic can't be swapped out.
- Encrypted and authenticated transport to the client.

### Other future work
- **Full video streaming** through the KV260's hardened Video Codec Unit (H.264/H.265) or a PL JPEG encoder, so compression happens in hardware.
- **Ultra-low-latency transport:** RTP or MJPEG over UDP from a C/C++ daemon.
- **More PII classes:** IDs, passports, documents, screens, license plates.
- **Fine-grained redaction:** mask only the number band or digits instead of the whole card.

---

## Board Setup (KV260)

Tested on Kria Ubuntu 22.04.4 (Python 3.10). The board talks to the laptop over a direct Ethernet cable; it has no internet, so dependencies are installed offline.

### One-time setup

**1. Flash and log in.** Flash Kria Ubuntu 22.04 to the SD card and boot. Log in over the micro-USB serial console (115200 baud) as `ubuntu` / `ubuntu`, then set a new password when prompted.

**2. Network (direct cable, static IPs).**
```sh
# Board
sudo nmcli con add type ethernet ifname eth0 con-name direct \
  ipv4.method manual ipv4.addresses 192.168.10.2/24 ipv6.method link-local autoconnect yes
sudo nmcli con up direct
```
On the Mac, open System Settings → Network → USB LAN adapter → Details → TCP/IP. Set Configure IPv4 to **Manual**, IP address `192.168.10.1`, subnet mask `255.255.255.0`, and leave Router empty. Use the IPv4 fields, not IPv6. Then, from the Mac:
```sh
ping 192.168.10.2
ssh-copy-id ubuntu@192.168.10.2
```

**3. Copy the project and install dependencies offline.** The board has no `ensurepip`, so the venv is created without pip and the system pip installs into it.
```sh
# Mac, from the repo root
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
After code changes, re-run only the `rsync` of the project.

**4. Camera.** With the board **powered off**, connect the AR1335 camera module to **J7 (IAS0)**. Seat the ribbon cable fully at both ends, with the contacts facing the connector's contacts, and close both latches.

### Every boot

```sh
ssh ubuntu@192.168.10.2
cd ~/HackUMass_Project
./scripts/kv260-camera-setup.sh
```
The script loads the Smart Camera firmware (`xmutil loadapp kv260-smartcam`), sets the camera chain to 1920×1080 NV12, and checks that frames arrive. When it reports success, start the stream:
```sh
.venv/bin/python -m faceshield.stream --gst "v4l2src device=/dev/video0 io-mode=mmap ! video/x-raw,format=NV12,width=1920,height=1080"
```
Open **http://192.168.10.2:8080** on the Mac. Faces are blacked out, and the whole frame is black when no face is detected (fail-closed). `Ctrl-C` stops it.

Other sources:
- `--test-pattern`: no camera; checks the network path.
- `--camera /dev/videoN`: a USB webcam.

**Test mode:** add `--test-no-blank` to see the camera picture when no face is detected (useful for aiming the camera and checking the detector). Detected faces are still blacked out, but frames without a detected face go out unmasked, so this mode is **not fail-closed**. The stream shows a red "TEST MODE: NOT FAIL-CLOSED" banner and the page says so. Never use it for a real demo of the privacy guarantee.

### Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `ssh` / `ping` time out | The Mac adapter's IPv4 isn't set to Manual, or the address went into the IPv6 fields. On the board, check `nmcli con show --active`. |
| No `/dev/video0` | The firmware isn't loaded. Run `sudo xmutil loadapp kv260-smartcam` (run `sudo xmutil unloadapp` first if another app is loaded). |
| Frame check times out, and `journalctl -k -b \| grep NACK` shows `NACK 0x6c` | The camera sensor isn't answering. Power off, then reseat the cable on J7 and check its orientation. |
| `STREAMON` fails with "Broken pipe" | The formats in the chain don't match. Re-run the setup script. |
| Python segfaults in `detectMultiScale` | The XRT OpenCL driver crashes OpenCV; `HaarFaceDetector` already disables OpenCL. Don't call `cv2.ocl.setUseOpenCL(True)`. |
| Every frame is black even with a face in view | Detection is too slow and every box goes stale. Keep `--detect-width 320` (full-resolution Haar takes about 740 ms per frame on the A53). |

The stream is unencrypted HTTP. It only ever carries scrubbed frames, but anyone on the cable's network can watch them.

---

## Tech Stack

| Layer | Tools |
|---|---|
| Hardware | AMD Kria KV260 (Zynq UltraScale+ MPSoC EV), MIPI camera |
| Platform | KV260 Smart Camera reference design, PetaLinux |
| FPGA / IP | Vivado, Vitis HLS / SystemVerilog, AXI-Stream, AXI-Lite |
| ML | YOLOv8n / YOLOv5n, Vitis AI (INT8), DPUCZDX8G; later FINN / hls4ml |
| App | Python, OpenCV, V4L2, `websockets` / FastAPI |
| Client | HTML5 / JavaScript (or React), WebSocket API |

## Team

- Connor McLaughlin
- Rajit Talluri
