"""Temporary CPU detector and the interface a future detector must implement."""

from typing import Iterable, Protocol, Tuple

import cv2

BoundingBox = Tuple[int, int, int, int]


class FaceDetector(Protocol):
    def detect(self, frame) -> Iterable[BoundingBox]:
        """Return pixel (x, y, width, height) boxes for this exact BGR frame."""
        ...


class HaarFaceDetector:
    """OpenCV's bundled frontal-face model; runs on the CPU, not the FPGA."""

    def __init__(self):
        model_path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
        self.cascade = cv2.CascadeClassifier(model_path)
        if self.cascade.empty():
            raise RuntimeError("Could not load OpenCV's bundled Haar face cascade")

    def detect(self, frame):
        # Haar features use intensity, so convert BGR to grayscale first.
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        gray = cv2.equalizeHist(gray)
        boxes = self.cascade.detectMultiScale(
            gray, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30)
        )
        return [tuple(int(value) for value in box) for box in boxes]
