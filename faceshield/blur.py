"""Detector-independent image processing."""

import math

import cv2


def blur_faces(frame, bounding_boxes, margin=0.15):
    """Return a copy with each (x, y, width, height) face region blurred.

    Coordinates are pixels; finite floating-point coordinates are accepted.
    Margin is a fraction of each box's width/height, added on each side.
    Malformed, non-finite, nonpositive, or off-image boxes are ignored.
    The input must be a nonempty OpenCV BGR image. Detection misses are
    unchanged: this function alone cannot guarantee anonymity.
    """
    if not math.isfinite(margin) or margin < 0:
        raise ValueError("margin must be a finite, nonnegative number")
    if frame is None or frame.ndim != 3 or frame.shape[2] != 3 or frame.size == 0:
        raise ValueError("frame must be a nonempty BGR image")

    processed = frame.copy()
    for box in bounding_boxes:
        clipped = clip_box(box, frame.shape, margin)
        if clipped is None:
            continue
        left, top, right, bottom = clipped
        region = processed[top:bottom, left:right]
        # Reduce detail to at most 8x8 samples first. This makes the smoothing
        # strong even for large close-up faces without huge Gaussian kernels.
        small = cv2.resize(region, (min(8, right - left), min(8, bottom - top)),
                           interpolation=cv2.INTER_AREA)
        small = cv2.GaussianBlur(small, (5, 5), sigmaX=2)
        processed[top:bottom, left:right] = cv2.resize(
            small, (right - left, bottom - top), interpolation=cv2.INTER_LINEAR
        )
    return processed


def clip_box(box, frame_shape, margin):
    """Return integer (left, top, right, bottom) for an (x, y, w, h) box.

    The margin is added and the result clipped to the frame. Returns None for
    malformed, non-finite, nonpositive, or off-image boxes.
    """
    try:
        x, y, width, height = (float(value) for value in box)
    except (TypeError, ValueError, OverflowError):
        return None
    if not all(math.isfinite(v) for v in (x, y, width, height)):
        return None
    if width <= 0 or height <= 0:
        return None
    image_height, image_width = frame_shape[:2]
    # Reject boxes entirely outside the frame before expanding their margin.
    if x >= image_width or y >= image_height or x + width <= 0 or y + height <= 0:
        return None
    # Clip before rounding so even very large finite boxes remain safe.
    left = math.floor(max(0, x - width * margin))
    top = math.floor(max(0, y - height * margin))
    right = math.ceil(min(image_width, x + width + width * margin))
    bottom = math.ceil(min(image_height, y + height + height * margin))
    if right <= left or bottom <= top:
        return None
    return left, top, right, bottom
