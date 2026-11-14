"""Headless board pipeline: capture -> detect -> scrub in place -> JPEG -> network.

Run on the KV260 with:  python -m faceshield.stream --gst "<pipeline>"
then open http://<board-ip>:8080 in a browser on the laptop.

Only JPEG bytes encoded from the scrubbed frame are handed to the network
side (FrameBroadcaster). The HTTP server never sees a raw frame.
"""

import argparse
import json
import math
import shlex
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2
import numpy as np

from .detectors import HaarFaceDetector
from .scrubber import FailClosedScrubber

BOUNDARY = b"faceshieldframe"


# --- Frame sources -----------------------------------------------------------

class OpenCVSource:
    """V4L2/USB camera through OpenCV (index like 0, or a path like /dev/video0)."""

    def __init__(self, device, width=640, height=480):
        device = int(device) if str(device).isdigit() else device
        self.capture = cv2.VideoCapture(device)
        if not self.capture.isOpened():
            raise RuntimeError(f"Could not open camera {device!r}")
        self.capture.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        self.capture.set(cv2.CAP_PROP_FRAME_HEIGHT, height)

    def read(self):
        ok, frame = self.capture.read()
        if not ok or frame is None or frame.size == 0:
            raise RuntimeError("Camera stopped delivering frames")
        return frame

    def close(self):
        self.capture.release()


class GstSource:
    """Raw BGR frames from a gst-launch-1.0 pipeline over a pipe.

    The pip OpenCV build has no GStreamer support, so the pipeline runs as a
    subprocess. `pipeline` is everything before the conversion to BGR, e.g.
    "v4l2src device=/dev/video0 io-mode=mmap".
    """

    def __init__(self, pipeline, width=640, height=480):
        self.shape = (height, width, 3)
        self.frame_bytes = width * height * 3
        # Scale first (still in the camera's format, e.g. NV12), then convert
        # the small frame to BGR. Converting full 1080p first took a whole A53
        # core and capped the stream at ~2 FPS; this order is ~3.6x faster.
        command = (
            ["gst-launch-1.0", "-q"] + shlex.split(pipeline)
            + ["!", "videoscale", "n-threads=2", "!",
               f"video/x-raw,width={width},height={height}",
               "!", "videoconvert", "n-threads=2", "!",
               f"video/x-raw,format=BGR,width={width},height={height}",
               "!", "fdsink", "fd=1", "sync=false"]
        )
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                        bufsize=self.frame_bytes * 2)

    def read(self):
        data = self.process.stdout.read(self.frame_bytes)
        if data is None or len(data) != self.frame_bytes:
            code = self.process.poll()
            raise RuntimeError(f"GStreamer pipeline stopped (exit code {code})")
        return np.frombuffer(data, np.uint8).reshape(self.shape).copy()

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()


class TestPatternSource:
    """Synthetic frames for testing the network path without a camera."""

    def __init__(self, width=640, height=480, fps=15):
        self.width, self.height, self.period = width, height, 1 / fps
        self.count = 0

    def read(self):
        time.sleep(self.period)
        self.count += 1
        frame = np.zeros((self.height, self.width, 3), np.uint8)
        frame[:, :, 1] = np.linspace(40, 200, self.width, dtype=np.uint8)
        x = (self.count * 8) % self.width
        cv2.rectangle(frame, (x, 100), (x + 80, 220), (255, 255, 255), -1)
        return frame

    def close(self):
        pass


# --- Pipeline and broadcaster -------------------------------------------------

class FrameBroadcaster:
    """Holds the latest scrubbed JPEG and wakes up every waiting client."""

    def __init__(self):
        self._condition = threading.Condition()
        self._jpeg = None
        self._sequence = 0
        self.closed = False
        self.stats = {"frames": 0, "blanked": 0, "faces_masked": 0, "fps": 0.0,
                      "error": None, "test_mode": False}

    def publish(self, jpeg_bytes):
        with self._condition:
            self._jpeg = jpeg_bytes
            self._sequence += 1
            self._condition.notify_all()

    def close(self, error=None):
        with self._condition:
            self.closed = True
            self.stats["error"] = error
            self._condition.notify_all()

    def wait_next(self, last_sequence, timeout=5.0):
        """Return (sequence, jpeg) newer than last_sequence, or (None, None) if closed."""
        with self._condition:
            self._condition.wait_for(
                lambda: self.closed or self._sequence > last_sequence, timeout)
            if self.closed or self._sequence <= last_sequence:
                return None, None
            return self._sequence, self._jpeg


TEST_BANNER = "TEST MODE: NOT FAIL-CLOSED"


def run_pipeline(source, detector, scrubber, broadcaster, stop, jpeg_quality=80):
    """Capture, scrub in place, encode, publish. Stops the stream on any error."""
    params = [int(cv2.IMWRITE_JPEG_QUALITY), jpeg_quality]
    test_mode = not scrubber.blank_when_empty
    broadcaster.stats["test_mode"] = test_mode
    started, count = time.monotonic(), 0
    try:
        while not stop.is_set():
            frame = source.read()
            captured_at = time.monotonic()
            scrubber.update(detector.detect(frame), captured_at)
            frame, passed = scrubber.scrub(frame)  # raw pixels destroyed here
            if test_mode:  # burned into the image so a screenshot can't hide it
                cv2.putText(frame, TEST_BANNER, (10, 30), cv2.FONT_HERSHEY_SIMPLEX,
                            0.8, (0, 0, 255), 2, cv2.LINE_AA)
            ok, buffer = cv2.imencode(".jpg", frame, params)
            if not ok:
                raise RuntimeError("JPEG encoding failed")
            broadcaster.publish(buffer.tobytes())
            count += 1
            stats = broadcaster.stats
            stats["frames"] += 1
            stats["blanked"] += 0 if passed or test_mode else 1
            stats["faces_masked"] += 1 if passed else 0
            elapsed = time.monotonic() - started
            if elapsed >= 2:
                stats["fps"] = round(count / elapsed, 1)
                started, count = time.monotonic(), 0
        broadcaster.close()
    except Exception as error:  # fail closed: stop streaming, never fall back
        broadcaster.close(error=str(error))
        raise
    finally:
        source.close()


# --- HTTP server ----------------------------------------------------------------

PAGE = b"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FaceShield</title><style>
body{margin:0;background:#111;color:#ddd;font:14px system-ui;display:flex;
flex-direction:column;align-items:center;gap:8px;padding:16px}
img{max-width:100%;background:#000;border:1px solid #333}
</style></head><body>
<img src="/stream" alt="scrubbed camera stream">
<div id="s">connecting...</div>
<script>
setInterval(async()=>{try{const s=await (await fetch('/stats')).json();
document.getElementById('s').textContent=s.error?('stopped: '+s.error):
(s.test_mode?'TEST MODE (not fail-closed) | ':'')+
`${s.fps} fps | ${s.frames} frames | ${s.faces_masked} with a face masked | ${s.blanked} blanked (no current face)`;}
catch(e){document.getElementById('s').textContent='board unreachable';}},1000);
</script></body></html>"""


def make_handler(broadcaster):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _send(self, body, content_type, status=200):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/":
                self._send(PAGE, "text/html; charset=utf-8")
            elif self.path == "/stats":
                self._send(json.dumps(broadcaster.stats).encode(), "application/json")
            elif self.path == "/frame.jpg":
                _, jpeg = broadcaster.wait_next(0)
                if jpeg is None:
                    self._send(b"no frame", "text/plain", 503)
                else:
                    self._send(jpeg, "image/jpeg")
            elif self.path == "/stream":
                self._stream()
            else:
                self._send(b"not found", "text/plain", 404)

        def _stream(self):
            self.send_response(200)
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=" + BOUNDARY.decode())
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            sequence = 0
            try:
                while True:
                    sequence, jpeg = broadcaster.wait_next(sequence)
                    if jpeg is None:
                        return
                    self.wfile.write(b"--" + BOUNDARY + b"\r\nContent-Type: image/jpeg\r\n"
                                     + f"Content-Length: {len(jpeg)}\r\n\r\n".encode()
                                     + jpeg + b"\r\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return

    return Handler


def serve(source, detector, scrubber, host="0.0.0.0", port=8080, jpeg_quality=80):
    """Run the pipeline in a thread and serve until interrupted or the pipeline fails."""
    broadcaster = FrameBroadcaster()
    stop = threading.Event()
    server = ThreadingHTTPServer((host, port), make_handler(broadcaster))
    server.daemon_threads = True
    failure = []

    def pipeline():
        try:
            run_pipeline(source, detector, scrubber, broadcaster, stop, jpeg_quality)
        except Exception as error:
            failure.append(error)
        finally:
            server.shutdown()

    worker = threading.Thread(target=pipeline, daemon=True)
    worker.start()
    print(f"FaceShield streaming on http://{host}:{port}/  (Ctrl-C to stop)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()
        worker.join(timeout=5)
    if failure:
        raise RuntimeError(f"pipeline stopped: {failure[0]}")


def main():
    parser = argparse.ArgumentParser(description="FaceShield headless stream server")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--camera", help="OpenCV camera index or /dev/videoN")
    group.add_argument("--gst", help="GStreamer source pipeline, e.g. 'v4l2src device=/dev/video0'")
    group.add_argument("--test-pattern", action="store_true", help="Synthetic frames, no camera")
    parser.add_argument("--host", default="0.0.0.0", help="Address to listen on (default: all)")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--quality", type=int, default=80, help="JPEG quality 1-100")
    parser.add_argument("--margin", type=float, default=0.15)
    parser.add_argument("--max-age", type=float, default=0.25)
    parser.add_argument("--detect-width", type=int, default=320)
    parser.add_argument("--test-no-blank", action="store_true",
                        help="TEST ONLY: show frames with no detected face instead of "
                             "blanking them (faces that are detected are still masked). "
                             "Not fail-closed; labelled on the stream.")
    args = parser.parse_args()
    if not math.isfinite(args.margin) or args.margin < 0:
        parser.error("--margin must be finite and nonnegative")
    if not math.isfinite(args.max_age) or args.max_age <= 0:
        parser.error("--max-age must be finite and positive")
    if not 1 <= args.quality <= 100:
        parser.error("--quality must be 1-100")

    try:
        if args.test_pattern:
            source = TestPatternSource(args.width, args.height)
        elif args.gst:
            source = GstSource(args.gst, args.width, args.height)
        else:
            source = OpenCVSource(args.camera, args.width, args.height)
        if args.test_no_blank:
            print("WARNING: --test-no-blank: frames with no detected face are sent "
                  "unmasked. Test mode only.", file=sys.stderr, flush=True)
        serve(source, HaarFaceDetector(detect_width=args.detect_width),
              FailClosedScrubber(margin=args.margin, max_age_s=args.max_age,
                                 blank_when_empty=not args.test_no_blank),
              args.host, args.port, args.quality)
    except (RuntimeError, OSError, cv2.error) as error:
        print(f"FaceShield: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
