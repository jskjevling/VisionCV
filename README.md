# VisionCV

A [VCV Rack](https://vcvrack.com) module that turns a live camera feed into
control voltage: brightness, RGB channel levels, motion, and position
tracking (brightest spot, or a chosen color blob).

**Status: Phase 2, macOS only.** The full pipeline works end-to-end and has
been verified both standalone (device enumeration, thread lifecycle, live
analysis against a real webcam) and inside VCV Rack 2 Pro (module
instantiates, camera opens, live preview updates, no crashes on repeated
add/remove). Windows/Linux builds and a resolution/FPS menu are the
remaining known gaps — see "Roadmap" below.

**Requires a one-time local patch to your VCV Rack installation — see
"macOS camera permission" below before building.** Not distributed through
the VCV Library in this state; see `DEPLOYMENT.md` if you need to set this
up on additional machines.

## macOS camera permission (required, one-time per machine)

VCV Rack's own `Info.plist` doesn't declare `NSCameraUsageDescription`,
because the official app was never built to touch a camera. macOS's privacy
system (TCC) enforces this at the **host app** level, not the plugin level
— so without this key, Rack gets killed outright (`SIGABRT`) the moment
VisionCV requests camera access, no matter how carefully the plugin defers
that request to a background thread. This isn't fixable from plugin code;
it requires patching the installed Rack app bundle:

```sh
sudo cp "/Applications/VCV Rack 2 Pro.app/Contents/Info.plist" ~/Rack-Info.plist.orig-backup
sudo /usr/libexec/PlistBuddy -c "Add :NSCameraUsageDescription string 'VisionCV needs camera access to generate CV from live video.'" "/Applications/VCV Rack 2 Pro.app/Contents/Info.plist"
sudo codesign --force --deep --sign - "/Applications/VCV Rack 2 Pro.app"
```

This needs to be redone after every Rack update (updates overwrite the app
bundle). See `DEPLOYMENT.md` for the full per-machine setup checklist,
including how to undo it.

We've asked VCV support whether `NSCameraUsageDescription` could be added to
an official Rack release — that's the only fix that wouldn't require this
local patch (or a separate camera-capture helper process; see "VCV Library
packaging" below). No response yet.

## What it does

Click the live preview (or right-click the module, or click the camera-name
display) to pick a connected camera. It analyzes frames on a background
thread — never on the audio thread — and outputs:

| Output | Range | Meaning |
|---|---|---|
| Brightness | 0–10V | Luminance-weighted average brightness |
| Red / Green / Blue | 0–10V each | Per-channel average level |
| Motion | 0–10V | Frame-to-frame difference magnitude, gained by the Motion Sens knob |
| Pos X / Pos Y | −5..+5V | Position of the tracked point; 0V = frame center, up = positive |
| Found | 0/10V gate | High whenever the tracker has a confident lock — always high in brightest-spot mode, only high in color-blob mode when this frame actually matched the target color |

**Mode** switch chooses what Pos X/Y (and Found) track: the brightest pixel
in frame, or a chosen color (Hue knob sets the target, Tolerance sets the
accepted band width — or just click a color directly on the live preview,
which also switches Mode to color-blob automatically). **Smoothing** applies
a one-pole filter to all outputs. **Freeze** (button or CV gate) holds the
current output values, including Found.

The selected camera is saved with the patch (matched back by the camera's
own unique ID on load, not by index, since USB device indices aren't
stable across reboots/reconnects) and restored automatically.

## Building (macOS, Apple Silicon)

Requires Xcode Command Line Tools, Homebrew, and the VCV Rack SDK.

**Important:** put the Rack SDK somewhere on a path with **no spaces**. GNU
Make's `include` directive can't handle spaces in the path, and this repo
itself lives under `.../VCV Rack/...` (has a space) — that's fine for the
plugin's *own* build, but `RACK_DIR` must point elsewhere. This repo was
built against the SDK at `~/dev-tools/Rack-SDK`.

```sh
# 1. Dependencies for local dev (OpenCV) and building OpenPnP Capture (cmake)
brew install cmake pkg-config opencv

# 2. Rack SDK (match your Rack version; check uname -m for arch)
mkdir -p ~/dev-tools && cd ~/dev-tools
curl -fLO https://vcvrack.com/downloads/Rack-SDK-2.6.6-mac-arm64.zip
unzip Rack-SDK-2.6.6-mac-arm64.zip
export RACK_DIR=~/dev-tools/Rack-SDK   # add to your shell profile

# 3. Build OpenPnP Capture (camera capture library) from source
git clone https://github.com/openpnp/openpnp-capture ~/dev-tools/openpnp-capture
cd ~/dev-tools/openpnp-capture && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4

# 4. Vendor its header + built dylib into this plugin (dep/ is gitignored —
#    this step is what populates it; not committed to the repo)
cd "<this repo>"
mkdir -p dep/openpnp-capture/include dep/openpnp-capture/lib
cp ~/dev-tools/openpnp-capture/include/openpnp-capture.h dep/openpnp-capture/include/
cp ~/dev-tools/openpnp-capture/build/libopenpnp-capture.dylib dep/openpnp-capture/lib/
cp ~/dev-tools/openpnp-capture/LICENSE.txt dep/openpnp-capture/LICENSE-openpnp-capture.txt
install_name_tool -id "@loader_path/dep/openpnp-capture/lib/libopenpnp-capture.dylib" \
  dep/openpnp-capture/lib/libopenpnp-capture.dylib

# 5. Build and install into Rack's user plugin folder
make -j4
make install
```

Then launch VCV Rack — VisionCV should appear in the module browser under
its own brand. First launch will prompt for camera permission (macOS).

The Makefile deliberately does **not** use `pkg-config --libs opencv` as-is:
Homebrew's OpenCV package links all ~60 of its built submodules, which made
Rack take 30+ seconds to load this plugin on startup. Instead it links only
`opencv_core`, `opencv_imgproc`, and (OpenCV 5+) `opencv_geometry` — see the
comment in the Makefile if you need to adjust this for a different OpenCV
version.

## Architecture

- `src/capture/CameraWorker` — owns the camera (via OpenPnP Capture) and a
  background thread that continuously grabs and analyzes frames. Every
  single call into OpenPnP Capture — including creating the capture context
  and enumerating devices, not just grabbing frames — happens exclusively on
  that worker thread. On macOS, creating a capture context triggers the
  camera-authorization flow, which needs its calling thread's run loop free
  to complete; calling any of this from Rack's UI/main thread (e.g. directly
  in a constructor) deadlocks that thread and gets the process killed. Every
  other method on `CameraWorker` just posts a request or reads a cache —
  never touches the capture library directly — so it's safe to call from
  wherever Rack calls it (module construction, the audio thread, the
  right-click menu).
- `src/analysis/FrameAnalyzer` — the actual OpenCV math: brightness, RGB
  means, frame-diff motion, brightest-spot/color-blob position tracking.
- `src/shared/FrameBuffer` — mutex-guarded handoff of the latest analysis
  result from the worker thread to the audio thread.
- `src/shared/Thumbnail` — same idea, for the downscaled RGBA preview image
  handed to the UI thread. Regenerated every 3rd captured frame (~10fps),
  since resizing/color-converting every frame at full camera rate is wasted
  cost for a small UI preview.
- `src/VisionCV.cpp` — the Rack `Module`/`ModuleWidget`. `process()` never
  touches the camera or OpenCV directly — it only reads the latest result,
  smooths it, and writes voltages. The widget draws its own panel text at
  runtime (`ModuleWidget::draw()`, via NanoVG) rather than baking `<text>`
  into the panel SVG, since VCV Rack's SVG renderer (NanoSVG) doesn't render
  `<text>` as real glyphs.

`CameraWorker`/`FrameAnalyzer` have no Rack dependency and can be exercised
standalone (useful for testing without the Rack GUI).

## VCV Library packaging

Not currently planned for macOS: submitting as-is would mean every user's
first camera selection crashes Rack, since the Info.plist patch above can't
travel with a plugin — only the host app's own bundle can satisfy macOS's
per-process TCC check. Two paths could change that: VCV adding the
entitlement to an official release (asked, no response yet), or splitting
camera capture into a separate helper process with its own signed bundle/
entitlement (a real fix, but a meaningfully sized new subsystem — a second
binary, an IPC protocol, its own permission prompt). Neither is in progress.

This is a macOS-specific problem (TCC has no equivalent on Windows/Linux for
a plain desktop app), so it doesn't block Library packaging for those
platforms once they're built. In the meantime, direct/GitHub distribution
(what `DEPLOYMENT.md` sets up) works regardless of Library status — plenty
of Rack community plugins are distributed that way.

## Roadmap

1. ~~macOS prototype: capture, all 4 analysis features, camera-select menu~~ ✅
2. ~~Polish: live preview thumbnail + click-to-pick color, Found gate,
   restore selected camera across patch load~~ ✅
3. Resolution/FPS selection menu; graceful reconnect UX if a USB camera is
   unplugged mid-session
4. Windows static build (cross-compiled via `rack-plugin-toolchain`)
5. Linux static build
6. Packaging for the VCV Library (Windows/Linux only — see above)

## License

BSD-3-Clause (see `LICENSE`). Links against OpenCV (Apache-2.0) and OpenPnP
Capture (MIT) at build time.
