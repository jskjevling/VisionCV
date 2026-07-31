#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <openpnp-capture.h>
#include "../shared/CameraTypes.hpp"

class SharedCameraSession;


/** Process-wide singleton owning the single CapContext — and its single
background thread — used for every physical camera any VisionCV module
instance touches, no matter how many instances are open. See
SharedCameraSession's class comment for why camera access needs to be
shared across instances rather than owned per-module.

Every single Cap_* call for every open camera happens exclusively on this
one background thread, for the same reason a single module's capture used
to confine them to its own worker thread: on macOS, creating a capture
context triggers AVFoundation's camera-authorization flow, which needs its
calling thread's run loop free to complete — calling any of this from
Rack's UI/main thread deadlocks that thread and gets the whole app killed
by the OS watchdog. Every public method here only posts a request or reads
a cache — never touches Cap_* directly — so they're safe to call from
wherever Rack calls them (module construction, the audio thread, the
right-click menu). */
class CameraSessionManager {
public:
	static CameraSessionManager& instance();

	/** Cached device list, refreshed roughly once a second. Safe to call
	from any thread, including Rack's UI thread. */
	std::vector<CameraDeviceInfo> listDevices();

	/** Get (creating if necessary) the shared session for the device at
	this index into the most recent listDevices() result. The returned
	session starts disconnected and connects asynchronously on the manager
	thread; if another module instance already has this same physical
	device open, this returns that *same* session (ref-counted) instead of
	opening a second, conflicting stream. Returns nullptr if the index is
	out of range for the current device cache. */
	std::shared_ptr<SharedCameraSession> acquireByIndex(int index);

	/** Same, but resolved by unique device ID (used to restore a patch's
	saved camera selection). Safe to call before the device cache has ever
	been populated — resolution happens asynchronously on the manager
	thread, retried until the device turns up (or forever, if it never
	does — e.g. a saved selection for hardware that's no longer attached). */
	std::shared_ptr<SharedCameraSession> acquireByUniqueId(const std::string& uniqueId);

private:
	CameraSessionManager();
	~CameraSessionManager();
	CameraSessionManager(const CameraSessionManager&) = delete;
	CameraSessionManager& operator=(const CameraSessionManager&) = delete;

	void threadLoop();
	void refreshDeviceCache();
	CapFormatID pickFormat(SharedCameraSession& session, CapDeviceID index, uint32_t& outWidth, uint32_t& outHeight, uint32_t& outFps);

	CapContext ctx = nullptr;
	std::thread thread;
	std::atomic<bool> running{true};

	std::mutex deviceCacheMutex;
	std::vector<CameraDeviceInfo> deviceCache;

	struct SessionEntry {
		CapStream stream = -1;
		std::weak_ptr<SharedCameraSession> handle;
	};
	std::mutex sessionsMutex;
	std::map<std::string, SessionEntry> sessions; // keyed by unique device ID
};
