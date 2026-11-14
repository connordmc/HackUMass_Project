**Redaction of sensitive information at the camera, in hardware, before any pixel reaches the network.**

Built on the AMD Kria KV260 for [Hackathon Name].

---

## The Problem

When a camera feed is sent over a network, everything in the frame can be intercepted. If someone photographs a credit card and an attacker is tapping the link between the camera and the receiving machine, the card number is exposed. Encryption in transit helps, but the receiving host, its software, and its memory all still see the raw image.

## The Idea

Move the privacy boundary onto the edge device. Frames are captured and processed on the Kria KV260, and sensitive regions are masked **in the FPGA fabric** on the output path. Only sanitized frames are ever handed to the network stack. The unredacted image never leaves the board.

**Immediate prototype:** detect a card-shaped object in the camera feed and blank out the card before the image is sent to the laptop.

## Threat Model

| Guaranteed in the hackathon build | Planned after the hackathon | Out of scope |
|---|---|---|
| Passive or active interception of the Ethernet link | Root or kernel-level compromise of Linux on the Kria (see [Next Steps](#next-steps-after-the-hackathon)) | Physical attacks (probing, JTAG, bitstream extraction) |
| Compromised or buggy software on the receiving laptop | | Side-channel attacks on the FPGA |
| The network-facing application sending an unmasked frame (it only ever receives scrubbed buffers) | | A malicious bitstream |

**Hackathon guarantee:** no unmasked card is ever transmitted off the board.

---

## System Architecture (Hackathon Build)

```
┌──────────────────────────────── Kria KV260 (Zynq UltraScale+ MPSoC) ────────────────────────────────┐
│                                                                                                      │
│  Programmable Logic (PL)                                                                             │
│  ┌────────┐ MIPI CSI-2 ┌───────────┐       AXI-Stream        ┌──────────────────┐                     │
│  │ Camera │───────────▶│ ISP/video │──────────┬─────────────▶│   Scrubber IP    │──────────┐          │
│  └────────┘            │ pipeline  │          │              │ (mask bbox,      │          │          │
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

- Model: YOLOv8n or YOLOv5n, quantized to INT8 with Vitis AI, running on the DPU (DPUCZDX8G).
- Input: raw frames DMA'd into a **reserved DDR region** (`reserved-memory` in the device tree). The region holds detector input only and is never exposed to the network application.
- Output: card bounding box `(x_min, y_min, x_max, y_max)` plus a valid flag, written to the scrubber's AXI-Lite registers.

### 3. Scrubbing: inline in programmable logic (AXI-Stream)

- A custom IP block (HLS or SystemVerilog) sits on the AXI-Stream path between the camera pipeline and the **clean** frame-buffer DMA.
- Pixels inside the current bounding box (plus a safety margin) are overwritten with a black fill or blur as they stream through.
- The output buffer that Linux and the network app read from has already been scrubbed in hardware. There is no software masking step that could be skipped or bypassed.

### 4. Timing and fail-closed behavior

- **Previous-frame boxes:** the DPU detects on frame N−1, and the scrubber applies that box to frame N (about 16 ms later at 60 FPS), padded to cover small motion.
- **Fail-closed by default:** the scrubber blanks or drops the **entire frame** unless a valid, current detection is present. If detection is late, missing or stale, nothing unmasked goes out.
- **Snapshot mode for the demo:** hold the card steady, trigger capture, let the DPU update the box, and send the next scrubbed frame.

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
- [ ] INT8 YOLO-nano card detector on the DPU, reading from the reserved DDR region
- [ ] PL scrubber IP on the clean output path, box set over AXI-Lite
- [ ] Fail-closed gate: full-frame blank when there is no valid detection
- [ ] Python WebSocket server sending sanitized JPEG snapshots
- [ ] Browser client on the laptop displaying received frames

### Phase 2: Stretch (if time allows)
- [ ] Classical-vision card detector in HLS (color/edge threshold → largest rectangular blob → ISO card aspect-ratio check, ~1.586) reading a downscaled copy of the frame held in on-chip BRAM. If this works, it replaces the DPU path and enables the full hardware-isolation claim during the demo.
- [ ] Continuous throttled streaming (5–10 FPS) instead of single snapshots
- [ ] Blur instead of solid fill

### Fallback
If the AXI-Stream scrubber integration stalls, prototype end to end with OpenCV masking on the ARM cores, then move the mask into the PL.

---

## Next Steps (After the Hackathon)

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
- **More PII classes:** IDs, passports, documents, screens, faces, license plates.
- **Fine-grained redaction:** mask only the number band or digits instead of the whole card.

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

- [Name] — [role]
