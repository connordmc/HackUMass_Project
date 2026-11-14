"""Run with: python -m faceshield"""

import argparse
import math
import sys
import time

import cv2

from .scrubber import FailClosedScrubber
from .detectors import FaceDetector, HaarFaceDetector


def run_camera(detector: FaceDetector, camera_index=0, margin=0.15, max_age=0.25):
    """Capture, detect, scrub in place (fail-closed), display only the scrubbed frame."""
    scrubber = FailClosedScrubber(margin=margin, max_age_s=max_age)
    camera = cv2.VideoCapture(camera_index)
    window_name = "FaceShield - CPU prototype - q to quit"
    try:
        if not camera.isOpened():
            raise RuntimeError(
                f"Could not open camera {camera_index}. Close other camera apps, "
                "check camera permissions, or try --camera 1."
            )
        # These are requests; the camera backend may choose another resolution.
        camera.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        camera.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
        while True:
            success, frame = camera.read()
            if not success or frame is None or frame.size == 0:
                raise RuntimeError("Camera stopped delivering frames. Check its connection.")
            captured_at = time.monotonic()
            # Do not catch detector errors and fall back to showing a raw frame.
            scrubber.update(detector.detect(frame), captured_at)
            # In place: after this call the raw pixels no longer exist.
            frame, _ = scrubber.scrub(frame)
            cv2.imshow(window_name, frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
            if cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) < 1:
                break
    finally:
        camera.release()
        cv2.destroyAllWindows()


def main():
    parser = argparse.ArgumentParser(description="FaceShield local CPU webcam prototype")
    parser.add_argument("--camera", type=int, default=0, help="Camera index (default: 0)")
    parser.add_argument("--margin", type=float, default=0.15,
                        help="Fraction of face size added on each side (default: 0.15)")
    parser.add_argument("--max-age", type=float, default=0.25,
                        help="Seconds before a detection is stale and the frame is blanked (default: 0.25)")
    parser.add_argument("--detect-width", type=int, default=320,
                        help="Width of the downscaled copy used for detection; 0 = full size "
                             "(default: 320, needed for speed on the KV260)")
    args = parser.parse_args()
    if args.detect_width < 0:
        parser.error("--detect-width must be 0 or positive")
    if not math.isfinite(args.margin) or args.margin < 0:
        parser.error("--margin must be finite and nonnegative")
    if not math.isfinite(args.max_age) or args.max_age <= 0:
        parser.error("--max-age must be finite and positive")
    try:
        run_camera(HaarFaceDetector(detect_width=args.detect_width), args.camera, args.margin, args.max_age)
    except KeyboardInterrupt:
        return 0
    except (RuntimeError, cv2.error) as error:
        print(f"FaceShield: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
