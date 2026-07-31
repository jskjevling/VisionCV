#include "CameraSessionManager.hpp"
#include "SharedCameraSession.hpp"
#include <algorithm>
#include <cstdlib>
#include <chrono>

using Clock = std::chrono::steady_clock;


CameraSessionManager& CameraSessionManager::instance() {
	static CameraSessionManager manager;
	return manager;
}

CameraSessionManager::CameraSessionManager() {
	// Deliberately does nothing but start the thread: Cap_createContext()
	// must run on that thread, not here -- see the class comment in
	// CameraSessionManager.hpp.
	thread = std::thread(&CameraSessionManager::threadLoop, this);
}

CameraSessionManager::~CameraSessionManager() {
	running = false;
	if (thread.joinable())
		thread.join();
}

std::vector<CameraDeviceInfo> CameraSessionManager::listDevices() {
	std::lock_guard<std::mutex> lock(deviceCacheMutex);
	return deviceCache;
}

std::shared_ptr<SharedCameraSession> CameraSessionManager::acquireByUniqueId(const std::string& uniqueId) {
	if (uniqueId.empty())
		return nullptr;

	std::lock_guard<std::mutex> lock(sessionsMutex);
	std::map<std::string, SessionEntry>::iterator it = sessions.find(uniqueId);
	if (it != sessions.end()) {
		std::shared_ptr<SharedCameraSession> existing = it->second.handle.lock();
		if (existing)
			return existing; // another module instance already has this camera open
	}

	// Not open yet: register a new (not-yet-connected) session. The manager
	// thread notices entries with stream < 0 each tick and resolves/opens
	// them, retrying until the device turns up if it isn't enumerable yet.
	std::shared_ptr<SharedCameraSession> session = std::make_shared<SharedCameraSession>();
	{
		std::lock_guard<std::mutex> nameLock(session->nameMutex);
		session->deviceUniqueId = uniqueId;
	}
	SessionEntry entry;
	entry.stream = -1;
	entry.handle = session;
	sessions[uniqueId] = entry;
	return session;
}

std::shared_ptr<SharedCameraSession> CameraSessionManager::acquireByIndex(int index) {
	std::vector<CameraDeviceInfo> devices;
	{
		std::lock_guard<std::mutex> lock(deviceCacheMutex);
		devices = deviceCache;
	}
	if (index < 0 || static_cast<size_t>(index) >= devices.size())
		return nullptr;
	return acquireByUniqueId(devices[index].uniqueId);
}

void CameraSessionManager::refreshDeviceCache() {
	std::vector<CameraDeviceInfo> devices;
	uint32_t count = Cap_getDeviceCount(ctx);
	devices.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		CameraDeviceInfo info;
		const char* name = Cap_getDeviceName(ctx, i);
		const char* uid = Cap_getDeviceUniqueID(ctx, i);
		info.name = name ? name : ("Camera " + std::to_string(i));
		info.uniqueId = uid ? uid : "";
		devices.push_back(info);
	}
	std::lock_guard<std::mutex> lock(deviceCacheMutex);
	deviceCache = devices;
}

CapFormatID CameraSessionManager::pickFormat(SharedCameraSession& session, CapDeviceID index,
	uint32_t& outWidth, uint32_t& outHeight, uint32_t& outFps) {
	uint32_t targetW, targetH, targetFps;
	{
		std::lock_guard<std::mutex> lock(session.preferredMutex);
		targetW = session.preferredWidth;
		targetH = session.preferredHeight;
		targetFps = session.preferredFps;
	}

	int32_t numFormats = Cap_getNumFormats(ctx, index);
	CapFormatID best = 0;
	long bestScore = -1;
	outWidth = targetW;
	outHeight = targetH;
	outFps = 0;

	std::vector<CameraFormatOption> formats;
	for (int32_t i = 0; i < numFormats; i++) {
		CapFormatInfo info;
		if (Cap_getFormatInfo(ctx, index, static_cast<CapFormatID>(i), &info) != CAPRESULT_OK)
			continue;

		CameraFormatOption opt;
		opt.width = info.width;
		opt.height = info.height;
		opt.fps = info.fps;
		formats.push_back(opt);

		long score = std::labs(static_cast<long>(info.width) - static_cast<long>(targetW))
			+ std::labs(static_cast<long>(info.height) - static_cast<long>(targetH));
		if (targetFps > 0)
			score += std::labs(static_cast<long>(info.fps) - static_cast<long>(targetFps)) * 10;
		else if (info.fps < 15)
			score += 100000; // no fps preference: still deprioritize very low frame rates

		if (bestScore < 0 || score < bestScore) {
			bestScore = score;
			best = static_cast<CapFormatID>(i);
			outWidth = info.width;
			outHeight = info.height;
			outFps = info.fps;
		}
	}

	std::sort(formats.begin(), formats.end(), [](const CameraFormatOption& a, const CameraFormatOption& b) {
		if (a.width != b.width) return a.width > b.width;
		if (a.height != b.height) return a.height > b.height;
		return a.fps > b.fps;
	});
	formats.erase(std::unique(formats.begin(), formats.end(), [](const CameraFormatOption& a, const CameraFormatOption& b) {
		return a.width == b.width && a.height == b.height && a.fps == b.fps;
	}), formats.end());

	{
		std::lock_guard<std::mutex> lock(session.formatMutex);
		session.formatCache = formats;
	}

	return best;
}

void CameraSessionManager::threadLoop() {
	// Cap_createContext() triggers macOS's camera-authorization flow — must
	// happen here, on this dedicated thread, never on any caller's thread.
	ctx = Cap_createContext();

	// Force an immediate device-list refresh on the first iteration.
	Clock::time_point lastEnumerate = Clock::now() - std::chrono::seconds(10);

	while (running.load()) {
		Clock::time_point now = Clock::now();
		if (now - lastEnumerate > std::chrono::milliseconds(1000)) {
			refreshDeviceCache();
			lastEnumerate = now;
		}

		std::vector<CameraDeviceInfo> devices;
		{
			std::lock_guard<std::mutex> lock(deviceCacheMutex);
			devices = deviceCache;
		}

		bool didWork = false;
		std::vector<std::string> toErase;
		{
			std::lock_guard<std::mutex> lock(sessionsMutex);
			for (std::map<std::string, SessionEntry>::iterator it = sessions.begin(); it != sessions.end(); ++it) {
				const std::string& uid = it->first;
				SessionEntry& entry = it->second;
				std::shared_ptr<SharedCameraSession> session = entry.handle.lock();

				if (!session) {
					// No module instance holds a reference anymore.
					if (entry.stream >= 0)
						Cap_closeStream(ctx, entry.stream);
					toErase.push_back(uid);
					continue;
				}

				if (entry.stream < 0) {
					// Not yet open (or just closed for a format change) --
					// resolve the device's current index and open it.
					for (size_t i = 0; i < devices.size(); i++) {
						if (devices[i].uniqueId != uid)
							continue;
						CapDeviceID idx = static_cast<CapDeviceID>(i);
						uint32_t w = 0, h = 0, fps = 0;
						CapFormatID fmt = pickFormat(*session, idx, w, h, fps);
						CapStream s = Cap_openStream(ctx, idx, fmt);
						if (s >= 0) {
							entry.stream = s;
							session->stream = s;
							session->deviceIndex = idx;
							session->frameWidth = static_cast<int>(w);
							session->frameHeight = static_cast<int>(h);
							session->captureBuffer.assign(static_cast<size_t>(w) * h * 3, 0);
							session->frameCounter = 0;
							{
								std::lock_guard<std::mutex> nameLock(session->nameMutex);
								session->deviceName = devices[i].name;
							}
							{
								std::lock_guard<std::mutex> formatLock(session->formatMutex);
								session->currentFormat.width = w;
								session->currentFormat.height = h;
								session->currentFormat.fps = fps;
							}
							didWork = true;
						}
						break;
					}
					continue;
				}

				if (session->formatChangeRequested.exchange(false)) {
					Cap_closeStream(ctx, entry.stream);
					entry.stream = -1;
					session->stream = -1;
					session->connected = false;
					didWork = true;
					// Settling delay: OpenPnP Capture's macOS backend can
					// still deliver one more in-flight callback after a
					// stream closes (see historical note this class
					// replaces in CameraWorker). Reopens on a later tick via
					// the entry.stream < 0 branch above, now targeting the
					// new preferred format.
					std::this_thread::sleep_for(std::chrono::milliseconds(150));
					continue;
				}

				if (Cap_hasNewFrame(ctx, entry.stream)) {
					CapResult res = Cap_captureFrame(ctx, entry.stream, session->captureBuffer.data(),
						static_cast<uint32_t>(session->captureBuffer.size()));
					if (res == CAPRESULT_OK) {
						RawFrame frame;
						frame.rgb = std::make_shared<std::vector<uint8_t>>(session->captureBuffer);
						frame.width = session->frameWidth;
						frame.height = session->frameHeight;
						frame.frameCounter = ++session->frameCounter;
						session->frameBuffer.write(frame);
						session->connected = true;
					}
					else {
						session->connected = false;
					}
					didWork = true;
				}
			}
		}

		if (!toErase.empty()) {
			std::lock_guard<std::mutex> lock(sessionsMutex);
			for (size_t i = 0; i < toErase.size(); i++)
				sessions.erase(toErase[i]);
		}

		if (!didWork)
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	// Shutdown, still entirely on this thread.
	{
		std::lock_guard<std::mutex> lock(sessionsMutex);
		for (std::map<std::string, SessionEntry>::iterator it = sessions.begin(); it != sessions.end(); ++it) {
			if (it->second.stream >= 0)
				Cap_closeStream(ctx, it->second.stream);
		}
		sessions.clear();
	}
	if (ctx)
		Cap_releaseContext(ctx);
}
