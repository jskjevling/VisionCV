#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include "../analysis/FrameAnalyzer.hpp"
#include "../shared/FrameBuffer.hpp"
#include "../shared/Thumbnail.hpp"
#include "../shared/CameraTypes.hpp"

class SharedCameraSession;


/** Per-module analysis pipeline. Subscribes to a SharedCameraSession (via
CameraSessionManager) for whichever physical camera this module instance
has selected, and runs this module's own FrameAnalyzer — independent
MODE/HUE/TOLERANCE/etc. per instance — on the frames that session
publishes.

Actual camera I/O lives entirely in SharedCameraSession/
CameraSessionManager now: multiple module instances pointed at the same
physical camera share one real capture session instead of each opening
their own. Opening the same consumer/UVC webcam twice independently isn't
reliably supported by the hardware, and reconfiguring one such independent
stream's resolution could corrupt the other — this split is the fix for
that (previously a real, reproduced bug).

This class's own public API is unchanged from when it owned capture
directly, so the Module/ModuleWidget code using it didn't need to change. */
class CameraWorker {
public:
	using DeviceInfo = CameraDeviceInfo;
	using FormatOption = CameraFormatOption;

	CameraWorker();
	~CameraWorker();

	/** Returns the process-wide cached device list (refreshed roughly once
	a second). Safe to call from the UI thread at any time — never blocks,
	never touches the capture library directly. */
	std::vector<DeviceInfo> listDevices();

	/** Request subscribing to the camera at this index into the most recent
	listDevices() result. Non-blocking. Pass -1 to unsubscribe. */
	void selectDevice(int index);

	/** Request subscribing by unique device ID (used to restore a patch's
	saved selection). Non-blocking; safe to call before the device cache has
	ever been populated. */
	void selectDeviceByUniqueId(const std::string& uniqueId);

	/** Unsubscribe from the current camera, if any. Non-blocking. */
	void close();

	/** Update the analysis parameters this instance's analyzer reads each
	frame. Safe to call from the audio thread at any time. */
	void setAnalysisParams(const AnalysisParams& params);

	/** Get this instance's most recently analyzed frame's results (or a
	default, disconnected result if no camera has ever been selected). */
	AnalysisResult getLatestResult();

	/** Get this instance's most recently generated preview thumbnail (or a
	default, empty one — check width/height before using). */
	Thumbnail getLatestThumbnail();

	bool isConnected();
	std::string getDeviceName();
	std::string getDeviceUniqueId();

	/** Resolutions/frame-rates the currently subscribed camera reports.
	Empty if not subscribed to anything yet. */
	std::vector<FormatOption> listFormats();

	/** The format actually in use right now (or a zeroed FormatOption if
	not subscribed). For checkmarking the resolution menu. */
	FormatOption getCurrentFormat();

	/** Request a different resolution/frame rate for the camera this
	instance is subscribed to. Affects every other module instance sharing
	that same physical camera too — see SharedCameraSession's class comment
	for why that's the correct, hardware-honest behavior. No-op if not
	currently subscribed to anything. */
	void setPreferredFormat(uint32_t width, uint32_t height, uint32_t fps);

private:
	void analysisThreadLoop();

	std::mutex sessionMutex;
	std::shared_ptr<SharedCameraSession> session;

	// --- Cross-thread request queue: UI thread writes, analysis thread reads/clears ---
	std::mutex requestMutex;
	bool closeRequested = false;
	bool selectRequested = false;
	int requestedIndex = -1;
	bool selectByUidRequested = false;
	std::string requestedUid;

	std::mutex paramsMutex;
	AnalysisParams analysisParams;

	FrameAnalyzer analyzer;
	FrameBuffer frameBuffer;
	uint32_t analysisFrameCounter = 0;
	uint32_t lastSeenRawFrameCounter = 0;

	ThumbnailBuffer thumbnailBuffer;
	int thumbnailSkipCounter = 0;
	uint32_t thumbnailFrameCounter = 0;

	std::thread worker;
	std::atomic<bool> running{true};
};
