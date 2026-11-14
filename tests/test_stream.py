"""End-to-end stream tests over a real local socket, no camera needed."""

import json
import threading
import time
import unittest
import urllib.request
from http.server import ThreadingHTTPServer

import cv2
import numpy as np

from faceshield.scrubber import FailClosedScrubber
from faceshield.stream import (FrameBroadcaster, TestPatternSource, make_handler,
                               run_pipeline)


class StubDetector:
    def __init__(self, boxes):
        self.boxes = boxes

    def detect(self, frame):
        return self.boxes


class FailingSource:
    def read(self):
        raise RuntimeError("camera unplugged")

    def close(self):
        pass


class StreamTests(unittest.TestCase):
    test_mode = False

    def start(self, detector, source=None, test_mode=False):
        self.test_mode = test_mode
        self.broadcaster = FrameBroadcaster()
        self.stop = threading.Event()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(self.broadcaster))
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        source = source or TestPatternSource(160, 120, fps=50)
        self.worker = threading.Thread(
            target=lambda: self._run(source, detector), daemon=True)
        self.worker.start()
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"

    def _run(self, source, detector):
        try:
            run_pipeline(source, detector,
                         FailClosedScrubber(margin=0, blank_when_empty=not self.test_mode),
                         self.broadcaster, self.stop)
        except RuntimeError:
            pass

    def tearDown(self):
        self.stop.set()
        self.worker.join(timeout=2)
        self.server.shutdown()
        self.server.server_close()

    def fetch_frame(self):
        with urllib.request.urlopen(self.base + "/frame.jpg", timeout=5) as response:
            self.assertEqual(response.headers["Content-Type"], "image/jpeg")
            return cv2.imdecode(np.frombuffer(response.read(), np.uint8), cv2.IMREAD_COLOR)

    def test_no_face_streams_black_frames(self):
        self.start(StubDetector([]))
        frame = self.fetch_frame()
        self.assertEqual(frame.shape, (120, 160, 3))
        self.assertLess(frame.max(), 10)  # JPEG noise only

    def test_face_region_is_blacked_out_rest_visible(self):
        self.start(StubDetector([(0, 0, 40, 40)]))
        frame = self.fetch_frame()
        # JPEG ringing bleeds a little brightness in at the edges.
        self.assertLess(frame[8:32, 8:32].max(), 20)
        self.assertLess(frame[8:32, 8:32].mean(), 5)
        self.assertGreater(frame[60:, 80:].mean(), 30)

    def test_test_mode_shows_frame_without_face_and_says_so(self):
        self.start(StubDetector([]), test_mode=True)
        frame = self.fetch_frame()
        self.assertGreater(frame[60:, 80:].mean(), 30)  # picture, not black
        # Red banner burned into the top-left corner.
        banner = frame[10:35, 10:200]
        self.assertGreater(int(banner[:, :, 2].max()), 200)
        with urllib.request.urlopen(self.base + "/stats", timeout=5) as response:
            self.assertTrue(json.loads(response.read())["test_mode"])

    def test_mjpeg_stream_delivers_multiple_parts(self):
        self.start(StubDetector([(0, 0, 40, 40)]))
        with urllib.request.urlopen(self.base + "/stream", timeout=5) as response:
            self.assertIn("multipart/x-mixed-replace", response.headers["Content-Type"])
            data = b""
            while data.count(b"--faceshieldframe") < 3:
                data += response.read1(65536)

    def test_source_failure_closes_stream(self):
        self.start(StubDetector([(0, 0, 40, 40)]), source=FailingSource())
        self.worker.join(timeout=2)
        self.assertTrue(self.broadcaster.closed)
        self.assertIn("camera unplugged", self.broadcaster.stats["error"])
        with urllib.request.urlopen(self.base + "/stream", timeout=5) as response:
            self.assertEqual(response.read(), b"")


if __name__ == "__main__":
    unittest.main()
