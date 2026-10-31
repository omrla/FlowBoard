# FlowBoard

A whiteboard you draw on in the air. Your webcam tracks your hands, and you control the
cursor with Meta Quest-style hand gestures: pinch to click and draw, tap your fingers for
undo and the menu, grab to move the board.

By [@omrla](https://github.com/omrla). Windows only.

C++20, wxWidgets for the UI, and OpenCV for the camera and neural-network inference. Hand
tracking runs Google's MediaPipe palm detector and hand-landmark models (ONNX exports from
the [OpenCV Zoo](https://github.com/opencv/opencv_zoo)) on the graphics card through
[ONNX Runtime](https://onnxruntime.ai) with DirectML, which works with any DirectX 12 GPU
(AMD, NVIDIA or Intel). On a Radeon RX 6600 XT that takes each model from 15–30 ms on the
CPU to under 1 ms, so tracking keeps up with the camera's 30 frames a second with both
hands. Without a usable GPU it falls back to the CPU through OpenCV. The camera status at
the top right shows which one is in use. No cloud needed.

## Gestures

| Gesture | Action |
| --- | --- |
| Move your hand | Move the cursor (the ring shrinks as your pinch closes) |
| Pinch thumb + index | Click a button / put a dot |
| Pinch, hold and move | Draw with the current tool |
| Tap thumb + middle finger | Undo |
| Tap thumb + ring finger | Open / close the quick (radial) menu |
| Tap thumb + pinky | Clear the board (a thumb + middle undo brings it back) |
| Turn palm toward you + pinch | Recenter the view |
| Make a fist and move | Grab and pan the board |
| Pinch with both hands | Zoom: pull apart / push together |

## The window

- **Top bar:** the **File**, **View** and **Effects** menus, and two cards: **Gestures**
  (F1) and **Tips**. On the right: the camera status (frames per second and GPU or CPU;
  click it for details), the zoom (click to reset the view), and buttons that turn the
  camera on and off (F5) and show or hide the camera panel (F2).
- **Camera panel:** the camera preview with your hands' live state, which camera to use,
  how many hands to track, and the tracking sliders (hover one to see what it does).
- **On the board:** the tool rail on the left and the colour and size bar at the bottom,
  both big enough to pinch.

The window chrome is dark, like the board's toolbars, using wxWidgets' Windows dark mode.

## Ink effects

The **Effects** menu in the top bar changes how new strokes look (a dot on the tab means
one is on):

- **Rainbow ink:** each stroke cycles through the colours of the rainbow.
- **Neon glow:** strokes glow like neon tubes (best on the dark board).
- **Mirror drawing:** every stroke is mirrored across the dashed line down the middle of
  the board.
- **Vanishing ink:** strokes fade away after a moment and are never saved, like a laser
  pointer for presenting.

Effects can be combined. Rainbow and neon strokes keep their look in PNG, JPEG and PDF
exports.

Tools: pen, highlighter, eraser, line, rectangle, ellipse, pan. Pinch them on the left
rail. Pick colours and brush sizes on the bottom bar. The rail also has undo, redo,
clear and save.

## Saving

- **File → Save as… (Ctrl+S)** lets you pick any folder and file name, and save as a
  **PNG** or **JPEG** picture or a **PDF**. The PDF holds vector strokes, so it stays sharp
  at any zoom and prints cleanly.
- **File → Quick save folder** and **Quick save format** set where the save button on
  the board's left rail and **Ctrl+Shift+S** save, straight there with a timestamped
  name, since a hand can't drive a file dialog. Save as also remembers the last folder
  and format you used.
- Exports are cropped to your drawing, with a margin, and keep the board's background
  colour.
- Closing FlowBoard with unsaved changes asks whether to save first. **Save…** opens the
  Save as dialog, and cancelling it keeps FlowBoard open.

The mouse works too: left-drag draws, middle-drag pans, the wheel zooms and right-click
opens the quick menu. Keys: `P H E L R O` pick tools, `1`–`8` colours, `[` `]` size,
`Space` pan, `Ctrl+Z/Y` undo/redo, `Ctrl+D` dark board, `Ctrl+G` dot grid, `Ctrl+0`
reset view, `F1` gestures, `F2` camera panel, `F5` camera on/off, `F11` full screen.

Tips (the short version is in the top bar's **Tips** card):

- Sit about an arm's length from the camera and keep your hand in the middle of its
  view. A hand at the edge (often too low) is partly cut off and tracks poorly, so
  FlowBoard shows a hint telling you which way to move.
- **Reach** (camera panel) sets how much of the camera view maps onto the whole board.
  Lower it if you have to stretch.
- You can pinch and draw with your hand turned sideways or pointing at the camera.
  "Palm toward you" only counts once the back of your hand faces the camera (turned past
  about 120°).
- The ring-finger tap is the hardest gesture for the camera to see, because the middle
  finger partly hides the ring finger. Hold your hand up, palm toward the camera, and
  touch the thumb firmly to the tip of the ring finger.

## Build (Windows)

Requirements: Windows 10 or 11, and Visual Studio 2026 (version 18) or newer with the
*Desktop development with C++* workload. It includes vcpkg, so there's nothing else to
install.

```powershell
git clone https://github.com/omrla/FlowBoard.git
```

The first build also downloads
ONNX Runtime with DirectML (Microsoft's official NuGet package, about 12 MB, MIT licence)
into `third_party\onnxruntime` using `tools\fetch-onnxruntime.ps1`, and the two hand models
into `models\` using `tools\fetch-models.ps1` if they aren't there. DirectML itself is part
of Windows 10 1903 and later.

**Visual Studio:** open `FlowBoard.slnx`, choose **Release | x64** and press F5. On the
first build, vcpkg downloads and compiles OpenCV and wxWidgets into `vcpkg_installed\`.
This takes 15–20 minutes on a machine that hasn't built them before. The output is
`x64\Release\FlowBoard.exe`, with its DLLs and a `models` folder next to it. Debug builds
work too, but the unoptimised neural networks run about 10× slower, so use Release for
actual drawing.

**Command line:**

```powershell
.\build.ps1
```

This builds the same project with MSBuild, without opening Visual Studio, into
`x64\Release`. Add `-Config Debug` for a Debug build.

## App icon

The icon (a hand-tracking skeleton pinching and drawing a line) is `res/FlowBoard.svg`.
The build embeds it in the .exe, and the app draws its window and taskbar icon and the
top-bar logo from it at whatever size is needed. The .exe's own icon in Explorer is
`res/FlowBoard.ico`, made from the SVG. After editing the SVG, regenerate it and rebuild:

```powershell
x64\Release\FlowBoard.exe --make-icon res\FlowBoard.ico
```

wxWidgets draws the SVG with nanosvg, which supports gradients but not clipping or
filters, so keep the drawing inside the rounded tile.

## How it works

- `HandTracker` reads the webcam through Media Foundation on one thread and tracks on
  another, which always takes the newest frame. Media Foundation queues frames that
  nobody reads, so with a single thread every slow frame would be followed by stale ones.
  The palm detector finds new hands. After that, each hand is followed frame to frame
  from its own landmarks, as MediaPipe does, which is faster and steadier than detecting
  it again every frame. `LandmarkSmoother` steadies the joints with MediaPipe's landmark
  filter, which measures speed in hand sizes per second. Still joints hold steady, and
  moving ones follow almost without lag. The model scores "Left" assuming a mirrored
  selfie image; FlowBoard feeds it the camera's own view, where that same hand is the
  right one, so the raw score is read as "right hand". Palm direction depends on this, and
  getting it backwards makes an upright, palm-to-camera hand read as palm toward you.
  Each next crop is moved along with the hand's motion and widened by the move, so a fast
  hand stays inside it. If a hand is lost anyway, the palm detector searches the same
  frame, and a hand found again near where the lost one was heading keeps its id, so a
  stroke in progress carries on.
- `NeuralNet` runs one model on the GPU through ONNX Runtime + DirectML, or on the CPU
  through OpenCV DNN if the GPU can't be used. The tracker loads and warms up the models
  on its own thread, so preparing them for the GPU doesn't freeze the window.
- `HandModels` contains the pre- and post-processing for both models: SSD anchors,
  rotated hand crops, and mapping landmarks back to the image. A tracked hand's next crop
  uses MediaPipe's own recipe: a box around the wrist and lower finger joints, which
  jitter less than fingertips, doubled in size. Both models get area-averaged
  (anti-aliased) input, since sampling a 1080p frame down to 192 or 224 pixels with
  plain bilinear filtering turns fine detail into landmark jitter.
- `GestureEngine` normalises finger distances by palm size and debounces pinch, fist and
  palm-facing states with hysteresis. The distances come from the model's metric 3D
  landmarks, so a tilted hand, or a thumb that only covers the index fingertip on screen,
  doesn't fake a pinch. An index pinch needs the thumb clearly nearer the index tip than
  the middle and ring tips. Undo, the menu and clearing are deliberate taps: the thumb
  must first be apart from that finger, then land much nearer its tip than any other
  fingertip, so a relaxed, curled hand doesn't trigger them. Ending a pinch takes 4 frames
  of open fingers (and a wider opening while the hand moves fast), because recorded strokes showed
  the model's thumb flickering away for 2–3 frames mid-stroke. Palm direction is the
  palm's true 3D angle, so it means the same thing whether your fingers are spread or
  relaxed. A hand that drops out mid-stroke is held for 0.4 s. The cursor is smoothed
  with a One Euro filter. Its anchor mixes the pinch point with steadier joints, and a
  press uses the cursor position from just before the pinch closed, so pinching doesn't
  nudge the cursor.
- `Board` is the canvas: vector strokes on an infinite, zoomable board, an eraser that
  splits strokes, snapshot-based undo, the hand-operable dock and radial menu, and the
  Quest-style cursors and "ghost hands". It keeps inking while a pinch looks like it is
  opening, and if the release is confirmed it trims the stroke back to where the fingers
  began to open, so neither a flicker nor a release wobble shows. Ink effects are
  drawn as layered pen passes (glow) and short runs of changing colour (rainbow), the
  same for the screen, PNG/JPEG and PDF.
- `MainFrame` is the wxWidgets window: the `TopBar` (custom-drawn, with the menus as
  dropdown tabs, and `InfoCard` popups for the gestures, tips and camera details), the
  camera panel with the preview and tuning controls, and the keyboard shortcuts. `Chrome`
  holds the dark colours, turns on wxWidgets' dark mode and, on Windows 11, colours the
  title bar to match the top bar and rounds the popups' corners.

## Licence

FlowBoard is released under the [MIT licence](LICENSE).

It builds on other people's work, each under its own licence:

- [MediaPipe](https://github.com/google-ai-edge/mediapipe) palm detection and hand
  landmark models by Google, in the ONNX versions from the
  [OpenCV Zoo](https://github.com/opencv/opencv_zoo) (Apache 2.0)
- [OpenCV](https://opencv.org) (Apache 2.0)
- [wxWidgets](https://www.wxwidgets.org) (wxWindows Library Licence)
- [ONNX Runtime](https://onnxruntime.ai) with DirectML (MIT)

The models and ONNX Runtime aren't stored in this repository: the build downloads them.
