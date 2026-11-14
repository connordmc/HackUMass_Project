# FaceShield laptop prototype

This is the local CPU face-blurring prototype. The existing README describes
separate hardware plans; no FPGA integration, networking, or recording is
implemented here. Python 3.9+ is required.

## Windows / VS Code

Open this project folder in VS Code and open a PowerShell terminal. If `py` is
unavailable, install Python with its launcher/PATH option, then reopen VS Code,
or replace `py` below with the full path to your Python executable.

```powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m faceshield
```

In VS Code, run **Python: Select Interpreter** from the command palette and
select `.venv\Scripts\python.exe`. Virtual environment activation is optional;
the explicit commands above also avoid PowerShell activation policy issues.

Focus the video window and press **q** to quit. Closing the window also exits.
If capture fails, close Teams/Zoom/other camera apps and enable camera access
for desktop apps in Windows Settings. Try another camera with:

```powershell
.\.venv\Scripts\python.exe -m faceshield --camera 1 --margin 0.20
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

## Linux

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m faceshield
.venv/bin/python -m unittest discover -s tests -v
```

A graphical desktop and GUI-capable OpenCV build are needed for `imshow`.
Do not use `opencv-python-headless` for this windowed prototype. Installing on
the KV260 will depend on its OS and available OpenCV packages; this has not
been verified on the board.

## Code and data flow

- `faceshield/__main__.py`: `VideoCapture` reads BGR images; the loop detects,
  blurs, and passes only the processed copy to `imshow`. `waitKey` handles GUI
  events and the quit key. Camera and windows are released in `finally`.
- `faceshield/detectors.py`: the bundled Haar cascade detects frontal faces
  on grayscale images. `detectMultiScale` searches multiple face sizes;
  `minNeighbors` filters weak candidates. Output is pixel `(x, y, width, height)`.
- `faceshield/blur.py`: independent of detection. Adds a configurable margin,
  clips boxes, reduces each region to at most 8x8 samples, Gaussian-smooths it,
  and resizes it back. Detail reduction keeps large faces strongly obscured
  without costly full-size Gaussian kernels. The original frame is preserved.
- `tests/test_blur.py`: deterministic synthetic images, using standard-library
  `unittest` and NumPy (installed as an OpenCV dependency).

A future detector only needs `detect(frame)` returning pixel boxes for that
same frame. Normalize coordinates, undo model resize/letterboxing, and convert
corner coordinates in a separate adapter once the actual DPU output is known.
There is deliberately no speculative FPGA API here.

## Webcam acceptance check

1. Launch the app; confirm a live window appears and your face is strongly blurred.
2. Move nearer/farther and across the frame; check that the blurred region follows.
3. Add a second person; check both faces, including near the image edges.
4. Assess smoothness at the requested 640x480 resolution (the camera may choose
   another size). Actual FPS and blur quality need checking on your laptop.
5. Press `q`, relaunch, and close the window; verify both release the webcam.

This is not a privacy guarantee. Haar can miss profiles, fast motion, small
faces, and faces in poor lighting. Missed faces and frames with no detections
remain unchanged, and blur is not proof of anonymity. Exceptions terminate
the loop without displaying a raw fallback, but fail-closed behavior for
detection misses remains future work. Raw frames exist in application memory.

## KV260 build

The on-board version (IAS camera → DPU face detection → fail-closed black fill → hardware H.264
→ laptop) lives in [`kv260/`](kv260/README.md). This laptop prototype is
unchanged.
