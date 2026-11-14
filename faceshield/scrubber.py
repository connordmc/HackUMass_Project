"""Fail-closed, in-place scrubbing: the policy layer between detector and output."""

import math
import time

from .blur import clip_box


def corrupt_regions(frame, boxes, margin=0.15):
    """Irreversibly overwrite each (x, y, w, h) region of `frame` IN PLACE.

    Unlike blur_faces there is no copy: the original pixels are destroyed
    (solid fill), so no retained reference to this buffer can recover them.
    Returns the number of regions corrupted.
    """
    count = 0
    for box in boxes:
        clipped = clip_box(box, frame.shape, margin)
        if clipped is None:
            continue
        left, top, right, bottom = clipped
        frame[top:bottom, left:right] = 0
        count += 1
    return count


class FailClosedScrubber:
    """Only lets a frame out if a valid, current detection covers it.

    Every face box is remembered with the time of the frame it was found in
    and keeps being masked until it is older than `max_age_s`. So if the
    detector catches only some of the faces on a frame, the faces it missed
    stay masked by their recent boxes. A moving face can leave a short trail
    of masked boxes, which errs toward hiding more.

    The frame is modified in place and is blanked entirely when no face box
    younger than `max_age_s` exists (including before the first detection).
    """

    def __init__(self, margin=0.15, max_age_s=0.25, clock=time.monotonic):
        if not math.isfinite(margin) or margin < 0:
            raise ValueError("margin must be a finite, nonnegative number")
        if not math.isfinite(max_age_s) or max_age_s <= 0:
            raise ValueError("max_age_s must be finite and positive")
        self.margin = margin
        self.max_age_s = max_age_s
        self._clock = clock
        self._tracked = []  # list of (box, frame_time)

    def update(self, boxes, frame_time):
        """Record the face boxes found in the frame captured at `frame_time`."""
        self._tracked.extend((box, frame_time) for box in boxes)
        self._drop_stale(frame_time)

    def _current_boxes(self, now):
        return [box for box, t in self._tracked if 0 <= now - t <= self.max_age_s]

    def _drop_stale(self, now):
        # Boxes from the future are kept; _current_boxes ignores them.
        self._tracked = [(box, t) for box, t in self._tracked
                         if now - t <= self.max_age_s]

    def scrub(self, frame, now=None):
        """Scrub `frame` in place and return (frame, passed_through).

        passed_through is False when the whole frame was blanked.
        """
        now = self._clock() if now is None else now
        boxes = self._current_boxes(now)
        if boxes and corrupt_regions(frame, boxes, self.margin) > 0:
            return frame, True
        frame[:] = 0
        return frame, False
