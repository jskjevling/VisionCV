#pragma once
#include <vector>
#include <cstdint>
#include <memory>
#include <mutex>


/** A single captured RGB24 frame, published by a SharedCameraSession and
read by every module instance analyzing that physical camera's feed.

The pixel data is held by a `shared_ptr<const vector>` rather than copied
per read: the capturing thread publishes one immutable buffer per frame,
and every subscriber's read is just a cheap shared_ptr copy (a refcount
bump) instead of a deep copy of the frame — meaningful when several module
instances are all polling the same camera at once. */
struct RawFrame {
	std::shared_ptr<const std::vector<uint8_t>> rgb; // width * height * 3 bytes
	int width = 0;
	int height = 0;
	uint32_t frameCounter = 0;
};

class RawFrameBuffer {
public:
	void write(RawFrame frame) {
		std::lock_guard<std::mutex> lock(mutex);
		latest = std::move(frame);
	}

	RawFrame read() {
		std::lock_guard<std::mutex> lock(mutex);
		return latest;
	}

private:
	std::mutex mutex;
	RawFrame latest;
};
