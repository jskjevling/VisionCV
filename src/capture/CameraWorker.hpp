#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <openpnp-capture.h>
#include "../analysis/FrameAnalyzer.hpp"
#include "../shared/FrameBuffer.hpp"
#include "../shared/Thumbnail.hpp"


/** Owns a camera device (via OpenPnP Capture) and a background thread that
continuously grabs and analyzes frames.

Every single Cap_* call — including Cap_createContext() and device
enumeration, not just frame capture — happens exclusively on the worker
thread. On macOS, creating a capture context triggers AVFoundation's camera
authorization flow, which needs the calling thread's run loop free to
complete; calling it from Rack's UI/main thread (e.g. directly in this
class's constructor) deadlocks that thread and gets the whole app killed by
the OS watchdog. So the worker thread owns its CapContext/CapStream for its
entire lifetime, and every other method here only posts a request or reads a
cache — never touches Cap_* directly.
*/
class CameraWorker {
public:
	struct DeviceInfo {
		std::string name;
		std::string uniqueId;
	};

	CameraWorker();
	~CameraWorker();

	/** Returns the most recently cached device list (refreshed roughly once
	a second by the worker thread). Safe to call from the UI thread at any
	time — never blocks, never touches the capture library directly. */
	std::vector<DeviceInfo> listDevices();

	/** Request that the worker thread open a camera by its index into the
	most recent listDevices() result. Non-blocking: just posts the request:
	the actual open happens on the worker thread a few milliseconds later.
	Pass -1 to request closing the current device. */
	void selectDevice(int index);

	/** Request opening a camera by its unique device ID (used to restore a
	patch's saved selection). Non-blocking; resolved against the worker
	thread's own device cache, so it's safe to call before that cache has
	ever been populated — the request just waits until it has been. */
	void selectDeviceByUniqueId(const std::string& uniqueId);

	/** Request closing the current device, if any. Non-blocking. */
	void close();

	/** Update the analysis parameters the worker reads each frame. Safe to
	call from the audio thread at any time. */
	void setAnalysisParams(const AnalysisParams& params);

	/** Get the most recently analyzed frame's results (or a default,
	disconnected result if no camera has ever been opened). */
	AnalysisResult getLatestResult();

	/** Get the most recently generated preview thumbnail (or a default,
	empty one — check width/height before using). Refreshed at a lower rate
	than analysis results; see threadLoop(). */
	Thumbnail getLatestThumbnail();

	bool isConnected() const { return connected.load(); }
	std::string getDeviceName();
	std::string getDeviceUniqueId();

private:
	void threadLoop();
	/** All of the below are only ever touched from within threadLoop() —
	i.e. only from the worker thread. No mutex needed for them. */
	void refreshDeviceCache();
	void openDeviceInternal(int index);
	void closeStreamInternal();
	CapFormatID pickFormat(CapDeviceID index, uint32_t& outWidth, uint32_t& outHeight);

	CapContext ctx = nullptr;
	CapStream stream = -1;
	int frameWidth = 0;
	int frameHeight = 0;
	std::vector<uint8_t> rgbBuffer;
	uint32_t frameCounter = 0;

	// --- Cross-thread request queue: UI thread writes, worker thread reads/clears ---
	std::mutex requestMutex;
	bool closeRequested = false;
	bool selectRequested = false;
	int requestedIndex = -1;
	bool selectByUidRequested = false;
	std::string requestedUid;

	// --- Cross-thread caches: worker thread writes, UI thread reads ---
	std::mutex deviceCacheMutex;
	std::vector<DeviceInfo> deviceCache;

	std::mutex nameMutex;
	std::string deviceName;
	std::string deviceUniqueId;

	std::thread worker;
	std::atomic<bool> running{true};
	std::atomic<bool> connected{false};

	std::mutex paramsMutex;
	AnalysisParams analysisParams;

	FrameAnalyzer analyzer;
	FrameBuffer frameBuffer;

	// Preview thumbnail: regenerated every few frames (not every frame —
	// resizing/converting is comparatively expensive and a UI preview
	// doesn't need full frame rate).
	ThumbnailBuffer thumbnailBuffer;
	uint32_t thumbnailFrameCounter = 0;
	int thumbnailSkipCounter = 0;
};
