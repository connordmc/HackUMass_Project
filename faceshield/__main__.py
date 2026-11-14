"""Run with: python -m faceshield"""

import argparse
import math
import sys

import cv2

from .blur import blur_faces
from .detectors import FaceDetector, HaarFaceDetector


def run_camera(detector: FaceDetector, camera_index=0, margin=0.15):
    """Capture, detect, process, then display only the processed frame."""
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
            # Do not catch detector errors and fall back to showing a raw frame.
            boxes = detector.detect(frame)
            protected_frame = blur_faces(frame, boxes, margin=margin)
            cv2.imshow(window_name, protected_frame)
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
    args = parser.parse_args()
    if not math.isfinite(args.margin) or args.margin < 0:
        parser.error("--margin must be finite and nonnegative")
    try:
        run_camera(HaarFaceDetector(), args.camera, args.margin)
    except KeyboardInterrupt:
        return 0
    except (RuntimeError, cv2.error) as error:
        print(f"FaceShield: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
