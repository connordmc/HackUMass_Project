"""Synthetic tests: no webcam or face photos needed."""

import unittest

import numpy as np  # Installed by opencv-python; also the OpenCV image format.

from faceshield.blur import blur_faces


class BlurFacesTests(unittest.TestCase):
    def setUp(self):
        self.frame = np.random.default_rng(42).integers(0, 256, (80, 100, 3), dtype=np.uint8)

    def test_normal_box_reduces_detail_and_preserves_other_pixels(self):
        result = blur_faces(self.frame, [(20, 10, 40, 40)], margin=0)
        self.assertLess(result[10:50, 20:60].std(), self.frame[10:50, 20:60].std() / 4)
        mask = np.ones(self.frame.shape[:2], dtype=bool)
        mask[10:50, 20:60] = False
        np.testing.assert_array_equal(result[mask], self.frame[mask])

    def test_multiple_boxes(self):
        result = blur_faces(self.frame, [(5, 5, 20, 20), (60, 40, 20, 20)], margin=0)
        for x, y in [(5, 5), (60, 40)]:
            self.assertLess(result[y:y+20, x:x+20].std(), 20)

    def test_partial_boxes_clip_at_all_edges(self):
        result = blur_faces(self.frame, [(-10, -10, 30, 30), (90, 70, 30, 30)], margin=0)
        self.assertLess(result[:20, :20].std(), 20)
        self.assertLess(result[70:, 90:].std(), 20)
        np.testing.assert_array_equal(result[30:60], self.frame[30:60])

    def test_invalid_empty_and_offscreen_boxes_are_ignored(self):
        boxes = [(1, 1, 0, 20), (1, 1, 20, -1), (200, 200, 10, 10),
                 (float("nan"), 0, 10, 10), (0, 0, float("inf"), 10), (), None]
        np.testing.assert_array_equal(blur_faces(self.frame, boxes), self.frame)

    def test_no_faces_returns_independent_copy(self):
        result = blur_faces(self.frame, [])
        np.testing.assert_array_equal(result, self.frame)
        self.assertFalse(np.shares_memory(result, self.frame))

    def test_input_is_not_modified(self):
        before = self.frame.copy()
        result = blur_faces(self.frame, [(10, 10, 40, 40)])
        np.testing.assert_array_equal(self.frame, before)
        self.assertFalse(np.shares_memory(result, self.frame))

    def test_margin_expands_region(self):
        result = blur_faces(self.frame, [(20, 20, 20, 20)], margin=0.25)
        self.assertFalse(np.array_equal(result[15:20, 15:45], self.frame[15:20, 15:45]))
        np.testing.assert_array_equal(result[:15], self.frame[:15])

    def test_invalid_margin_rejected(self):
        for margin in [-0.1, float("nan"), float("inf")]:
            with self.assertRaises(ValueError):
                blur_faces(self.frame, [], margin=margin)


if __name__ == "__main__":
    unittest.main()
