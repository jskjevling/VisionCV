#pragma once
#include <vector>
#include <cstdint>
#include <mutex>


/** A small downscaled RGBA8 copy of the latest camera frame, for the
on-panel preview. Kept separate from AnalysisResult since it's much larger
data and updated at a lower rate (see CameraWorker's thumbnail throttling).
*/
struct Thumbnail {
	std::vector<uint8_t> rgba; // width * height * 4 bytes
	int width = 0;
	int height = 0;
	uint32_t frameCounter = 0;
};

/** Thread-safe handoff of the latest Thumbnail, mirroring FrameBuffer's
mutex-guarded-copy approach. The thumbnail is small (well under 100KB even
at a generous preview size) and only refreshed a few times a second, so the
copy-under-a-mutex cost here is negligible — no need for a fancier
lock-free double buffer. */
class ThumbnailBuffer {
public:
	void write(const Thumbnail& thumbnail) {
		std::lock_guard<std::mutex> lock(mutex);
		latest = thumbnail;
	}

	Thumbnail read() {
		std::lock_guard<std::mutex> lock(mutex);
		return latest;
	}

private:
	std::mutex mutex;
	Thumbnail latest;
};
