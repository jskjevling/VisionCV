#include "CameraWorker.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <cstdlib>
#include <algorithm>

using Clock = std::chrono::steady_clock;


CameraWorker::CameraWorker() {
	// Deliberately does nothing but start the thread: see the class comment
	// in CameraWorker.hpp for why Cap_createContext() must not run here.
	worker = std::thread(&CameraWorker::threadLoop, this);
}

CameraWorker::~CameraWorker() {
	running = false;
	if (worker.joinable())
		worker.join();
}

std::vector<CameraWorker::DeviceInfo> CameraWorker::listDevices() {
	std::lock_guard<std::mutex> lock(deviceCacheMutex);
	return deviceCache;
}

void CameraWorker::selectDevice(int index) {
	std::lock_guard<std::mutex> lock(requestMutex);
	selectByUidRequested = false;
	if (index < 0) {
		closeRequested = true;
		selectRequested = false;
	}
	else {
		requestedIndex = index;
		selectRequested = true;
		closeRequested = false;
	}
}

void CameraWorker::selectDeviceByUniqueId(const std::string& uniqueId) {
	if (uniqueId.empty())
		return;
	std::lock_guard<std::mutex> lock(requestMutex);
	requestedUid = uniqueId;
	selectByUidRequested = true;
	selectRequested = false;
	closeRequested = false;
}

void CameraWorker::close() {
	std::lock_guard<std::mutex> lock(requestMutex);
	selectByUidRequested = false;
	selectRequested = false;
	closeRequested = true;
}

void CameraWorker::setAnalysisParams(const AnalysisParams& params) {
	std::lock_guard<std::mutex> lock(paramsMutex);
	analysisParams = params;
}

AnalysisResult CameraWorker::getLatestResult() {
	AnalysisResult result = frameBuffer.read();
	result.connected = connected.load();
	return result;
}

Thumbnail CameraWorker::getLatestThumbnail() {
	return thumbnailBuffer.read();
}

std::string CameraWorker::getDeviceName() {
	std::lock_guard<std::mutex> lock(nameMutex);
	return deviceName;
}

std::string CameraWorker::getDeviceUniqueId() {
	std::lock_guard<std::mutex> lock(nameMutex);
	return deviceUniqueId;
}

CapFormatID CameraWorker::pickFormat(CapDeviceID index, uint32_t& outWidth, uint32_t& outHeight) {
	// Aim for something close to 640x480 at a usable frame rate: enough
	// resolution for stable analysis without wasting CPU on a full-res feed.
	const uint32_t targetW = 640;
	const uint32_t targetH = 480;

	int32_t numFormats = Cap_getNumFormats(ctx, index);
	CapFormatID best = 0;
	long bestScore = -1;
	outWidth = targetW;
	outHeight = targetH;

	for (int32_t i = 0; i < numFormats; i++) {
		CapFormatInfo info;
		if (Cap_getFormatInfo(ctx, index, static_cast<CapFormatID>(i), &info) != CAPRESULT_OK)
			continue;

		long score = std::labs(static_cast<long>(info.width) - static_cast<long>(targetW))
			+ std::labs(static_cast<long>(info.height) - static_cast<long>(targetH));
		if (info.fps < 15)
			score += 100000; // strongly deprioritize very low frame rates

		if (bestScore < 0 || score < bestScore) {
			bestScore = score;
			best = static_cast<CapFormatID>(i);
			outWidth = info.width;
			outHeight = info.height;
		}
	}
	return best;
}

void CameraWorker::refreshDeviceCache() {
	std::vector<DeviceInfo> devices;
	uint32_t count = Cap_getDeviceCount(ctx);
	devices.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		DeviceInfo info;
		const char* name = Cap_getDeviceName(ctx, i);
		const char* uid = Cap_getDeviceUniqueID(ctx, i);
		info.name = name ? name : ("Camera " + std::to_string(i));
		info.uniqueId = uid ? uid : "";
		devices.push_back(info);
	}
	std::lock_guard<std::mutex> lock(deviceCacheMutex);
	deviceCache = devices;
}

void CameraWorker::closeStreamInternal() {
	if (stream >= 0) {
		Cap_closeStream(ctx, stream);
		stream = -1;
	}
	connected = false;
	std::lock_guard<std::mutex> lock(nameMutex);
	deviceName.clear();
	deviceUniqueId.clear();
}

void CameraWorker::openDeviceInternal(int index) {
	closeStreamInternal();

	if (index < 0)
		return;

	uint32_t count = Cap_getDeviceCount(ctx);
	if (static_cast<uint32_t>(index) >= count)
		return;

	uint32_t w = 0, h = 0;
	CapFormatID fmt = pickFormat(static_cast<CapDeviceID>(index), w, h);
	CapStream s = Cap_openStream(ctx, static_cast<CapDeviceID>(index), fmt);
	if (s < 0)
		return;

	stream = s;
	frameWidth = static_cast<int>(w);
	frameHeight = static_cast<int>(h);
	rgbBuffer.assign(static_cast<size_t>(frameWidth) * frameHeight * 3, 0);

	const char* name = Cap_getDeviceName(ctx, static_cast<CapDeviceID>(index));
	const char* uid = Cap_getDeviceUniqueID(ctx, static_cast<CapDeviceID>(index));
	{
		std::lock_guard<std::mutex> lock(nameMutex);
		deviceName = name ? name : "";
		deviceUniqueId = uid ? uid : "";
	}

	analyzer.reset();
	frameCounter = 0;
	// Deliberately NOT setting connected = true here: opening the stream
	// only means the capture session was configured successfully, not that
	// frames are actually arriving yet. `connected` should reflect real
	// frame delivery, so it's only set true in threadLoop() after a
	// successful Cap_captureFrame() — otherwise the status light can show
	// "connected" while nothing is actually flowing.
}

void CameraWorker::threadLoop() {
	// Cap_createContext() triggers macOS's camera-authorization flow — must
	// happen here, on this dedicated thread, never on the caller's thread.
	ctx = Cap_createContext();

	// Force an immediate device-list refresh on the first iteration.
	Clock::time_point lastEnumerate = Clock::now() - std::chrono::seconds(10);

	while (running.load()) {
		Clock::time_point now = Clock::now();
		if (now - lastEnumerate > std::chrono::milliseconds(1000)) {
			refreshDeviceCache();
			lastEnumerate = now;
		}

		bool doClose = false, doSelect = false, doSelectUid = false;
		int idx = -1;
		std::string uid;
		{
			std::lock_guard<std::mutex> lock(requestMutex);
			if (closeRequested) {
				doClose = true;
				closeRequested = false;
			}
			if (selectRequested) {
				doSelect = true;
				idx = requestedIndex;
				selectRequested = false;
			}
			if (selectByUidRequested) {
				doSelectUid = true;
				uid = requestedUid;
				// Cleared below only if we can actually attempt a match
				// (cache non-empty); otherwise left pending to retry.
			}
		}

		if (doClose)
			closeStreamInternal();
		if (doSelect)
			openDeviceInternal(idx);
		if (doSelectUid) {
			std::vector<DeviceInfo> devices;
			{
				std::lock_guard<std::mutex> lock(deviceCacheMutex);
				devices = deviceCache;
			}
			if (!devices.empty()) {
				for (size_t i = 0; i < devices.size(); i++) {
					if (devices[i].uniqueId == uid) {
						openDeviceInternal(static_cast<int>(i));
						break;
					}
				}
				std::lock_guard<std::mutex> lock(requestMutex);
				selectByUidRequested = false;
			}
			// else: cache not populated yet, retry next iteration.
		}

		bool hasFrame = (stream >= 0) && Cap_hasNewFrame(ctx, stream) != 0;
		if (!hasFrame) {
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
			continue;
		}

		CapResult res = Cap_captureFrame(ctx, stream, rgbBuffer.data(), static_cast<uint32_t>(rgbBuffer.size()));
		if (res != CAPRESULT_OK) {
			connected = false;
			continue;
		}

		cv::Mat frame(frameHeight, frameWidth, CV_8UC3, rgbBuffer.data());
		AnalysisParams params;
		{
			std::lock_guard<std::mutex> lock(paramsMutex);
			params = analysisParams;
		}

		AnalysisResult result = analyzer.analyze(frame, params);
		result.frameCounter = ++frameCounter;
		frameBuffer.write(result);
		connected = true;

		// Regenerate the preview thumbnail every few frames — resize +
		// color conversion is comparatively expensive, and a UI preview
		// doesn't need to update at full camera frame rate.
		if (++thumbnailSkipCounter >= 3) {
			thumbnailSkipCounter = 0;
			const int maxDim = 160;
			float scale = (frameWidth >= frameHeight)
				? static_cast<float>(maxDim) / frameWidth
				: static_cast<float>(maxDim) / frameHeight;
			int tw = std::max(1, static_cast<int>(frameWidth * scale));
			int th = std::max(1, static_cast<int>(frameHeight * scale));

			cv::Mat small, rgba;
			cv::resize(frame, small, cv::Size(tw, th));
			cv::cvtColor(small, rgba, cv::COLOR_RGB2RGBA);

			Thumbnail thumb;
			thumb.width = tw;
			thumb.height = th;
			thumb.rgba.assign(rgba.data, rgba.data + static_cast<size_t>(tw) * th * 4);
			thumb.frameCounter = ++thumbnailFrameCounter;
			thumbnailBuffer.write(thumb);
		}
	}

	// Shutdown, still entirely on this thread.
	if (ctx && stream >= 0)
		Cap_closeStream(ctx, stream);
	if (ctx)
		Cap_releaseContext(ctx);
}
