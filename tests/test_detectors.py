"""Detector box-mapping tests: a stub cascade stands in for real faces."""

import unittest

import numpy as np

from faceshield.detectors import HaarFaceDetector


class StubCascade:
    def __init__(self, boxes):
        self.boxes = boxes
        self.seen_shape = None
        self.min_size = None

    def detectMultiScale(self, gray, scaleFactor, minNeighbors, minSize):
        self.seen_shape = gray.shape
        self.min_size = minSize
        return self.boxes


class DetectorScalingTests(unittest.TestCase):
    def test_downscaled_boxes_map_back_and_cover_original(self):
        d = HaarFaceDetector(detect_width=320)
        d.cascade = StubCascade([(10, 20, 31, 41)])
        (x, y, w, h), = d.detect(np.zeros((480, 640, 3), np.uint8))
        self.assertEqual(d.cascade.seen_shape, (240, 320))
        self.assertEqual(d.cascade.min_size, (15, 15))
        # Full-size face would be (20, 40, 62, 82); mapped box must cover it.
        self.assertLessEqual(x, 20)
        self.assertLessEqual(y, 40)
        self.assertGreaterEqual(x + w, 20 + 62)
        self.assertGreaterEqual(y + h, 40 + 82)

    def test_full_size_when_disabled_or_frame_smaller(self):
        for width in (0, 1000):
            d = HaarFaceDetector(detect_width=width)
            d.cascade = StubCascade([(5, 5, 30, 30)])
            d.detect(np.zeros((480, 640, 3), np.uint8))
            self.assertEqual(d.cascade.seen_shape, (480, 640))

    def test_invalid_parameters_rejected(self):
        for kwargs in ({"detect_width": -1}, {"scale_factor": 1.0}, {"min_face": 0}):
            with self.assertRaises(ValueError):
                HaarFaceDetector(**kwargs)


if __name__ == "__main__":
    unittest.main()
