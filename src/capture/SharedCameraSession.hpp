#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <openpnp-capture.h>
#include "../shared/CameraTypes.hpp"
#include "../shared/RawFrame.hpp"


/** One real, physical-camera capture stream, shared by every VisionCV
module instance pointed at the same device.

Multiple module instances that select the same physical camera get the
*same* SharedCameraSession (ref-counted via shared_ptr, see
CameraSessionManager) instead of each opening their own independent
stream. Opening the same consumer/UVC webcam from two truly independent
capture sessions is not reliably supported by the hardware/driver — most
webcams only support one active native resolution at a time across every
session using them. Reconfiguring one such independent session's resolution
while another was also open corrupted the other's frames (a real, live bug
this class exists to eliminate): the shared hardware's actual streaming
format changed underneath the other session, whose own buffer-size/stride
bookkeeping never learned about it.

All the actual Cap_* calls for this session happen on
CameraSessionManager's single background thread — this class only holds
thread-safe published state (raw frames, connection status, format info)
that any number of module analysis threads can read concurrently, plus a
couple of request flags the manager thread checks. Never call any Cap_*
function through this class directly; the fields below marked
"manager-thread-only" are exactly that. */
class SharedCameraSession {
public:
	/** Latest captured frame (or a default, empty one — check width/height
	— if nothing has been captured yet). Thread-safe, cheap to call. */
	RawFrame getLatestFrame() { return frameBuffer.read(); }

	bool isConnected() const { return connected.load(); }

	std::string getDeviceName() {
		std::lock_guard<std::mutex> lock(nameMutex);
		return deviceName;
	}

	std::string getDeviceUniqueId() {
		std::lock_guard<std::mutex> lock(nameMutex);
		return deviceUniqueId;
	}

	std::vector<CameraFormatOption> listFormats() {
		std::lock_guard<std::mutex> lock(formatMutex);
		return formatCache;
	}

	CameraFormatOption getCurrentFormat() {
		std::lock_guard<std::mutex> lock(formatMutex);
		return currentFormat;
	}

	/** Request a different resolution/frame rate. Affects every module
	instance sharing this physical camera — that's the correct, hardware-
	honest behavior for what is genuinely one piece of hardware, not a
	limitation. Non-blocking; the manager thread picks it up and reopens the
	stream with a brief settling delay (the same one that previously
	guarded against a stale-callback race in OpenPnP Capture's teardown). */
	void setPreferredFormat(uint32_t width, uint32_t height, uint32_t fps) {
		std::lock_guard<std::mutex> lock(preferredMutex);
		preferredWidth = width;
		preferredHeight = height;
		preferredFps = fps;
		formatChangeRequested = true;
	}

private:
	friend class CameraSessionManager;

	// --- Manager-thread-only: never touched from any other thread ---
	CapStream stream = -1;
	CapDeviceID deviceIndex = 0;
	int frameWidth = 0;
	int frameHeight = 0;
	std::vector<uint8_t> captureBuffer;
	uint32_t frameCounter = 0;

	// --- Thread-safe published state: manager thread writes, any analysis thread reads ---
	RawFrameBuffer frameBuffer;
	std::atomic<bool> connected{false};

	std::mutex nameMutex;
	std::string deviceName;
	std::string deviceUniqueId;

	std::mutex formatMutex;
	CameraFormatOption currentFormat;
	std::vector<CameraFormatOption> formatCache;

	// --- Cross-thread request: any thread writes (setPreferredFormat), manager thread reads/clears ---
	std::mutex preferredMutex;
	uint32_t preferredWidth = 640;
	uint32_t preferredHeight = 480;
	uint32_t preferredFps = 0;
	std::atomic<bool> formatChangeRequested{false};
};
