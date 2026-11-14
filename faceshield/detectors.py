"""Temporary CPU detector and the interface a future detector must implement."""

import math
from typing import Iterable, Protocol, Tuple

import cv2

BoundingBox = Tuple[int, int, int, int]


class FaceDetector(Protocol):
    def detect(self, frame) -> Iterable[BoundingBox]:
        """Return pixel (x, y, width, height) boxes for this exact BGR frame."""
        ...


class HaarFaceDetector:
    """OpenCV's bundled frontal-face model; runs on the CPU, not the FPGA."""

    def __init__(self, detect_width=320, scale_factor=1.2, min_face=30):
        """
        detect_width: run detection on a copy downscaled to this width (0 = full
            size). Boxes are mapped back to full-frame pixels. 320 keeps the
            KV260's Cortex-A53 at ~130 ms/frame instead of ~740 ms at 640.
        scale_factor: step between face sizes searched; larger is faster.
        min_face: smallest face to find, in full-frame pixels.
        """
        if detect_width < 0 or scale_factor <= 1 or min_face <= 0:
            raise ValueError("invalid detector parameters")
        # On Kria Ubuntu, AMD's XRT registers as an OpenCL driver and
        # detectMultiScale segfaults inside it. Force OpenCV's CPU path.
        cv2.ocl.setUseOpenCL(False)
        model_path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
        self.cascade = cv2.CascadeClassifier(model_path)
        if self.cascade.empty():
            raise RuntimeError("Could not load OpenCV's bundled Haar face cascade")
        self.detect_width = detect_width
        self.scale_factor = scale_factor
        self.min_face = min_face

    def detect(self, frame):
        # OpenCV's OpenCL switch is per-thread, so set it in the calling thread.
        cv2.ocl.setUseOpenCL(False)
        # Haar features use intensity, so convert BGR to grayscale first.
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        width = gray.shape[1]
        scale = 1.0
        if 0 < self.detect_width < width:
            scale = width / self.detect_width
            gray = cv2.resize(gray, (self.detect_width, round(gray.shape[0] / scale)),
                              interpolation=cv2.INTER_AREA)
        gray = cv2.equalizeHist(gray)
        min_side = max(12, round(self.min_face / scale))
        boxes = self.cascade.detectMultiScale(
            gray, scaleFactor=self.scale_factor, minNeighbors=3,
            minSize=(min_side, min_side)
        )
        # Map back to full-frame pixels, rounding outward so nothing shrinks.
        return [
            (int(x * scale), int(y * scale),
             math.ceil(w * scale) + 1, math.ceil(h * scale) + 1)
            for x, y, w, h in boxes
        ]
