# Deploying VisionCV to another macOS machine (exhibit setup)

This plugin is **not** distributed through the VCV Library in its current
state — it requires a local patch to VCV Rack itself (see below), which
isn't something that can ship through the normal plugin install path. This
doc is for manually preparing additional macOS machines (e.g. for an
exhibit) to run it.

Each machine needs three things: VCV Rack itself patched to declare camera
usage, the build dependencies, and the plugin built and installed. Budget
~20–30 minutes per machine, mostly spent on the OpenCV download/install.

## Why the Rack patch is required

VisionCV asks macOS for camera access. macOS's privacy system (TCC) checks
the **host application's** `Info.plist` for a `NSCameraUsageDescription` key
before allowing that — not the plugin's. VCV Rack was never built to touch a
camera, so it doesn't declare this key, and any plugin that requests camera
access will get Rack **killed outright** (`SIGABRT`, not a graceful denial)
until it's added. This is a one-time, per-machine, per-Rack-install patch —
there's no way around it without VCV shipping this key in an official
release, and no way to make it part of the plugin's own install.

**This patch must be redone after every VCV Rack update**, since updates
reinstall Rack's app bundle from scratch, overwriting the patched
`Info.plist`. Re-running the steps below takes under a minute.

## Per-machine setup

### 1. Install VCV Rack (if not already) and locate the app bundle

Confirm the exact path and name of the installed app — it may be
`VCV Rack 2 Pro.app`, `VCV Rack 2 Free.app`, etc. Substitute that path for
`<Rack.app>` everywhere below.

### 2. Patch Info.plist and re-sign

```sh
# Back up the original first (so a `cp` back + re-sign restores it if needed)
sudo cp "/Applications/<Rack.app>/Contents/Info.plist" ~/Rack-Info.plist.orig-backup

# Add the missing key
sudo /usr/libexec/PlistBuddy -c "Add :NSCameraUsageDescription string 'VisionCV needs camera access to generate CV from live video.'" "/Applications/<Rack.app>/Contents/Info.plist"

# Re-sign so macOS will still launch the modified bundle
sudo codesign --force --deep --sign - "/Applications/<Rack.app>"
```

You'll be prompted once for an admin password (sudo caches it briefly, so
all three commands run without repeating it if pasted together).

**To undo:** `sudo cp ~/Rack-Info.plist.orig-backup "/Applications/<Rack.app>/Contents/Info.plist" && sudo codesign --force --deep --sign - "/Applications/<Rack.app>"` — or just let Rack's normal update/reinstall process overwrite the bundle again.

### 3. Install build dependencies

```sh
# Xcode Command Line Tools (skip if already installed)
xcode-select --install

# Homebrew (skip if already installed)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Build/analysis dependencies
brew install cmake pkg-config opencv
```

### 4. Get the Rack SDK

Match the version to the installed Rack (check via Rack's About screen, or
`plutil -p "/Applications/<Rack.app>/Contents/Info.plist" | grep Version`).
Match the architecture to the machine: `mac-arm64` for Apple Silicon,
`mac-x64` for Intel (`uname -m` tells you which).

```sh
mkdir -p ~/dev-tools && cd ~/dev-tools
curl -fLO https://vcvrack.com/downloads/Rack-SDK-<version>-mac-<arch>.zip
unzip Rack-SDK-<version>-mac-<arch>.zip
export RACK_DIR=~/dev-tools/Rack-SDK   # add to ~/.zprofile so it persists
```

### 5. Build OpenPnP Capture (camera library) from source

Not available via Homebrew, so it's built once per machine:

```sh
git clone https://github.com/openpnp/openpnp-capture ~/dev-tools/openpnp-capture
cd ~/dev-tools/openpnp-capture && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4
```

### 6. Get this plugin's source onto the machine and build it

However you transfer the repo (USB drive, git clone, AirDrop the folder —
whatever's convenient without network access to your private repo):

```sh
cd /path/to/VisionCV   # wherever you copied it

mkdir -p dep/openpnp-capture/include dep/openpnp-capture/lib
cp ~/dev-tools/openpnp-capture/include/openpnp-capture.h dep/openpnp-capture/include/
cp ~/dev-tools/openpnp-capture/build/libopenpnp-capture.dylib dep/openpnp-capture/lib/
install_name_tool -id "@loader_path/dep/openpnp-capture/lib/libopenpnp-capture.dylib" \
  dep/openpnp-capture/lib/libopenpnp-capture.dylib

make -j4
make install
```

`make install` copies the built plugin into Rack's user plugin folder
automatically. Building fresh on each machine (rather than copying a
pre-built `plugin.dylib`) sidesteps CPU-architecture mismatches (Apple
Silicon vs. Intel) and Homebrew install-path differences between machines —
copying binaries around is not recommended here.

### 7. Verify

Launch Rack, drop a VisionCV module into a patch, right-click it to confirm
your camera(s) show up in the menu, select one, and patch an output into a
Scope to confirm it's live. The very first camera selection on a fresh
machine will trigger a one-time macOS permission dialog — approve it.

## Known limitations for exhibit use

- **macOS only.** Windows/Linux builds don't exist yet.
- **Rack updates break the patch.** If a machine auto-updates Rack between
  now and the exhibit, redo step 2. Consider disabling Rack's auto-update
  check on exhibit machines if that's a risk (Rack menu → check for updates
  settings), or verify the patch shortly before the exhibit opens.
- **Not sandboxed/notarized.** The re-signed Rack.app uses an ad-hoc
  signature, not VCV's original Developer ID signature. This is fine for
  locally running machines you control, but don't redistribute the patched
  app bundle itself — only the VisionCV plugin is yours to share; Rack is
  VCV's commercial product.
