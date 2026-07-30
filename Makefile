# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# --- Analysis: OpenCV (core+imgproc only, via pkg-config for local dev builds) ---
# Homebrew's opencv package name varies by major version (opencv4, opencv5, ...);
# probe for whichever is actually installed rather than hardcoding one.
OPENCV_PKG := $(shell pkg-config --exists opencv5 && echo opencv5 || (pkg-config --exists opencv4 && echo opencv4 || echo opencv))
FLAGS += $(shell pkg-config --cflags $(OPENCV_PKG))
# Deliberately NOT using `pkg-config --libs`: Homebrew's opencv.pc links all
# ~60 built submodules (dnn, gapi, stitching, ...), which took VCV Rack over
# 30 seconds to dlopen/verify on startup. Link only the modules this plugin's
# analysis code actually calls: core, imgproc, and (on OpenCV 5, which moved
# cv::moments() out of imgproc) geometry.
OPENCV_LIBDIR := $(shell pkg-config --variable=libdir $(OPENCV_PKG))
LDFLAGS += -L$(OPENCV_LIBDIR) -lopencv_core -lopencv_imgproc
ifneq ($(wildcard $(OPENCV_LIBDIR)/libopencv_geometry.*),)
	LDFLAGS += -lopencv_geometry
endif

# --- Capture: OpenPnP Capture (vendored under dep/, built from source — see README) ---
FLAGS += -Idep/openpnp-capture/include
LDFLAGS += -Ldep/openpnp-capture/lib -lopenpnp-capture

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
# For local macOS dev, libopenpnp-capture.dylib is bundled alongside plugin.dylib
# (see DISTRIBUTABLES below) with an install name relative to it (@loader_path/...),
# so an installed plugin folder is self-contained without a system-wide install.

# Add .cpp files to the build (src/ and its subdirectories)
SOURCES += $(wildcard src/*.cpp)
SOURCES += $(wildcard src/capture/*.cpp)
SOURCES += $(wildcard src/analysis/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)
DISTRIBUTABLES += dep/openpnp-capture/lib/libopenpnp-capture.dylib

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

# macOS: OpenPnP Capture's AVFoundation backend needs these frameworks.
ifdef ARCH_MAC
	LDFLAGS += -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation -framework IOKit
endif
