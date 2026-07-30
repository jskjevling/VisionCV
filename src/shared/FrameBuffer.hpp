#pragma once
#include <mutex>
#include "AnalysisResult.hpp"


/** Thread-safe handoff of the latest AnalysisResult from the camera worker
thread to the audio thread's process().

AnalysisResult is small (a handful of floats) and written/read only a few
hundred times a second at most, so a plain mutex-guarded copy is simpler and
just as fast in practice as a lock-free double buffer here — no need for the
extra complexity.
*/
class FrameBuffer {
public:
	void write(const AnalysisResult& result) {
		std::lock_guard<std::mutex> lock(mutex);
		latest = result;
	}

	AnalysisResult read() {
		std::lock_guard<std::mutex> lock(mutex);
		return latest;
	}

private:
	std::mutex mutex;
	AnalysisResult latest;
};
