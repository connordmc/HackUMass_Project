"""Fail-closed policy tests: synthetic frames, fake clock."""

import unittest

import numpy as np

from faceshield.scrubber import FailClosedScrubber, corrupt_regions


def make_frame():
    return np.random.default_rng(1).integers(1, 256, (80, 100, 3), dtype=np.uint8)


class ScrubberTests(unittest.TestCase):
    def test_corrupts_in_place_and_keeps_rest(self):
        original = make_frame()
        frame = original.copy()
        self.assertEqual(corrupt_regions(frame, [(20, 10, 40, 40)], margin=0), 1)
        self.assertTrue((frame[10:50, 20:60] == 0).all())
        np.testing.assert_array_equal(frame[60:], original[60:])

    def test_valid_current_detection_passes_through_scrubbed(self):
        s = FailClosedScrubber(margin=0, max_age_s=0.1)
        s.update([(20, 10, 40, 40)], frame_time=1.0)
        frame = make_frame()
        out, ok = s.scrub(frame, now=1.05)
        self.assertTrue(ok)
        self.assertIs(out, frame)
        self.assertTrue((frame[10:50, 20:60] == 0).all())
        self.assertTrue((frame[60:] != 0).any())

    def test_no_detection_yet_blanks_frame(self):
        frame = make_frame()
        _, ok = FailClosedScrubber().scrub(frame, now=1.0)
        self.assertFalse(ok)
        self.assertFalse(frame.any())

    def test_empty_detection_blanks_frame(self):
        s = FailClosedScrubber()
        s.update([], frame_time=1.0)
        frame = make_frame()
        _, ok = s.scrub(frame, now=1.0)
        self.assertFalse(ok)
        self.assertFalse(frame.any())

    def test_miss_reuses_last_box_until_stale(self):
        s = FailClosedScrubber(margin=0, max_age_s=0.1)
        s.update([(20, 10, 40, 40)], frame_time=1.0)
        s.update([], frame_time=1.05)
        frame = make_frame()
        self.assertTrue(s.scrub(frame, now=1.06)[1])
        self.assertTrue((frame[10:50, 20:60] == 0).all())
        s.update([], frame_time=1.2)
        frame = make_frame()
        self.assertFalse(s.scrub(frame, now=1.2)[1])
        self.assertFalse(frame.any())

    def test_multiple_faces_all_masked(self):
        s = FailClosedScrubber(margin=0, max_age_s=0.1)
        s.update([(5, 5, 20, 20), (60, 40, 20, 20)], frame_time=1.0)
        frame = make_frame()
        self.assertTrue(s.scrub(frame, now=1.0)[1])
        self.assertTrue((frame[5:25, 5:25] == 0).all())
        self.assertTrue((frame[40:60, 60:80] == 0).all())

    def test_partial_miss_keeps_missed_face_masked(self):
        s = FailClosedScrubber(margin=0, max_age_s=0.1)
        s.update([(5, 5, 20, 20), (60, 40, 20, 20)], frame_time=1.0)
        s.update([(5, 5, 20, 20)], frame_time=1.05)  # second face missed
        frame = make_frame()
        s.scrub(frame, now=1.05)
        self.assertTrue((frame[40:60, 60:80] == 0).all())
        # Once the missed face's box is stale, only the redetected one remains.
        s.update([(5, 5, 20, 20)], frame_time=1.12)
        frame = make_frame()
        s.scrub(frame, now=1.12)
        self.assertTrue((frame[5:25, 5:25] == 0).all())
        self.assertTrue((frame[40:60, 60:80] != 0).any())

    def test_only_invalid_boxes_blank_frame(self):
        s = FailClosedScrubber()
        s.update([(500, 500, 10, 10), (0, 0, -1, 5)], frame_time=1.0)
        frame = make_frame()
        self.assertFalse(s.scrub(frame, now=1.0)[1])
        self.assertFalse(frame.any())

    def test_stale_detection_blanks_frame(self):
        s = FailClosedScrubber(max_age_s=0.1)
        s.update([(20, 10, 40, 40)], frame_time=1.0)
        frame = make_frame()
        self.assertFalse(s.scrub(frame, now=1.2)[1])
        self.assertFalse(frame.any())

    def test_detection_from_the_future_blanks_frame(self):
        s = FailClosedScrubber()
        s.update([(20, 10, 40, 40)], frame_time=5.0)
        frame = make_frame()
        self.assertFalse(s.scrub(frame, now=1.0)[1])

    def test_bad_params_rejected(self):
        with self.assertRaises(ValueError):
            FailClosedScrubber(max_age_s=0)
        with self.assertRaises(ValueError):
            FailClosedScrubber(margin=float("nan"))


if __name__ == "__main__":
    unittest.main()
