"""Watch the censored FaceShield stream coming from the KV260.

The laptop only decodes and displays. All detection and masking happen on
the board, so this program never sees an unmasked frame.

    python kv260/laptop/viewer.py --host 192.168.1.50           # TCP (default)
    python kv260/laptop/viewer.py --sdp kv260/laptop/stream.sdp  # UDP / RTP

Needs opencv-python (already in the top-level requirements.txt).
"""

import argparse
import os
import sys
import time

# Must be set before cv2 is imported: low-latency FFmpeg input options.
os.environ.setdefault(
    "OPENCV_FFMPEG_CAPTURE_OPTIONS",
    "fflags;nobuffer|flags;low_delay|protocol_whitelist;file,udp,rtp,tcp",
)

import cv2  # noqa: E402

WINDOW = "FaceShield KV260 stream - q to quit"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--host", help="KV260 IP address (TCP mode)")
    source.add_argument("--sdp", help="SDP file for UDP/RTP mode")
    parser.add_argument("--port", type=int, default=5000)
    args = parser.parse_args()
    url = f"tcp://{args.host}:{args.port}" if args.host else args.sdp

    cv2.namedWindow(WINDOW, cv2.WINDOW_NORMAL)
    try:
        while True:
            print(f"connecting to {url} ...", file=sys.stderr)
            capture = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
            if not capture.isOpened():
                time.sleep(2)
                continue
            frames, started = 0, time.monotonic()
            while True:
                ok, frame = capture.read()
                if not ok:
                    print("stream interrupted, reconnecting", file=sys.stderr)
                    break
                cv2.imshow(WINDOW, frame)
                frames += 1
                if frames % 150 == 0:
                    rate = frames / (time.monotonic() - started)
                    print(f"receiving {rate:.1f} fps", file=sys.stderr)
                key = cv2.waitKey(1) & 0xFF
                closed = cv2.getWindowProperty(WINDOW, cv2.WND_PROP_VISIBLE) < 1
                if key == ord("q") or closed:
                    return 0
            capture.release()
            time.sleep(1)
    except KeyboardInterrupt:
        return 0
    finally:
        cv2.destroyAllWindows()


if __name__ == "__main__":
    sys.exit(main())
