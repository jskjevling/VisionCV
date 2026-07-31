#include "CameraWorker.hpp"
#include "CameraSessionManager.hpp"
#include "SharedCameraSession.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <algorithm>


CameraWorker::CameraWorker() {
	worker = std::thread(&CameraWorker::analysisThreadLoop, this);
}

CameraWorker::~CameraWorker() {
	running = false;
	if (worker.joinable())
		worker.join();
	// `session` (a shared_ptr) is destroyed along with this object here;
	// if this was the last subscriber to that physical camera,
	// CameraSessionManager's background thread notices on its next tick
	// and closes the underlying stream.
}

std::vector<CameraWorker::DeviceInfo> CameraWorker::listDevices() {
	return CameraSessionManager::instance().listDevices();
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
	result.connected = isConnected();
	return result;
}

Thumbnail CameraWorker::getLatestThumbnail() {
	return thumbnailBuffer.read();
}

bool CameraWorker::isConnected() {
	std::lock_guard<std::mutex> lock(sessionMutex);
	return session && session->isConnected();
}

std::string CameraWorker::getDeviceName() {
	std::lock_guard<std::mutex> lock(sessionMutex);
	return session ? session->getDeviceName() : std::string();
}

std::string CameraWorker::getDeviceUniqueId() {
	std::lock_guard<std::mutex> lock(sessionMutex);
	return session ? session->getDeviceUniqueId() : std::string();
}

std::vector<CameraWorker::FormatOption> CameraWorker::listFormats() {
	std::lock_guard<std::mutex> lock(sessionMutex);
	return session ? session->listFormats() : std::vector<FormatOption>();
}

CameraWorker::FormatOption CameraWorker::getCurrentFormat() {
	std::lock_guard<std::mutex> lock(sessionMutex);
	return session ? session->getCurrentFormat() : FormatOption();
}

void CameraWorker::setPreferredFormat(uint32_t width, uint32_t height, uint32_t fps) {
	std::lock_guard<std::mutex> lock(sessionMutex);
	if (session)
		session->setPreferredFormat(width, height, fps);
}

void CameraWorker::analysisThreadLoop() {
	while (running.load()) {
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
				selectByUidRequested = false;
			}
		}

		// Unlike the old per-instance capture design, acquiring a session
		// is a cheap, synchronous, non-blocking call (CameraSessionManager
		// resolves/opens it asynchronously on its own thread) — no retry
		// bookkeeping needed here.
		if (doClose) {
			std::lock_guard<std::mutex> lock(sessionMutex);
			session.reset();
			analyzer.reset();
			lastSeenRawFrameCounter = 0;
		}
		if (doSelect) {
			std::shared_ptr<SharedCameraSession> s = CameraSessionManager::instance().acquireByIndex(idx);
			std::lock_guard<std::mutex> lock(sessionMutex);
			session = s;
			analyzer.reset();
			lastSeenRawFrameCounter = 0;
		}
		if (doSelectUid) {
			std::shared_ptr<SharedCameraSession> s = CameraSessionManager::instance().acquireByUniqueId(uid);
			std::lock_guard<std::mutex> lock(sessionMutex);
			session = s;
			analyzer.reset();
			lastSeenRawFrameCounter = 0;
		}

		std::shared_ptr<SharedCameraSession> s;
		{
			std::lock_guard<std::mutex> lock(sessionMutex);
			s = session;
		}

		if (!s) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}

		RawFrame frame = s->getLatestFrame();
		if (frame.width <= 0 || frame.height <= 0 || !frame.rgb || frame.frameCounter == lastSeenRawFrameCounter) {
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
			continue;
		}
		lastSeenRawFrameCounter = frame.frameCounter;

		cv::Mat mat(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.rgb->data()));
		AnalysisParams params;
		{
			std::lock_guard<std::mutex> lock(paramsMutex);
			params = analysisParams;
		}

		AnalysisResult result = analyzer.analyze(mat, params);
		result.frameCounter = ++analysisFrameCounter;
		frameBuffer.write(result);

		// Regenerate the preview thumbnail every few analyzed frames —
		// resize + color conversion is comparatively expensive, and a UI
		// preview doesn't need to update at full camera frame rate.
		if (++thumbnailSkipCounter >= 3) {
			thumbnailSkipCounter = 0;
			const int maxDim = 160;
			float scale = (frame.width >= frame.height)
				? static_cast<float>(maxDim) / frame.width
				: static_cast<float>(maxDim) / frame.height;
			int tw = std::max(1, static_cast<int>(frame.width * scale));
			int th = std::max(1, static_cast<int>(frame.height * scale));

			cv::Mat small, rgba;
			cv::resize(mat, small, cv::Size(tw, th));
			cv::cvtColor(small, rgba, cv::COLOR_RGB2RGBA);

			Thumbnail thumb;
			thumb.width = tw;
			thumb.height = th;
			thumb.rgba.assign(rgba.data, rgba.data + static_cast<size_t>(tw) * th * 4);
			thumb.frameCounter = ++thumbnailFrameCounter;
			thumbnailBuffer.write(thumb);
		}
	}
}
