#pragma once
#include <cstdint>


/** Result of analyzing one camera frame.
Produced on the camera worker thread, consumed on the audio thread via FrameBuffer.
Must stay a small POD-ish struct (cheap to copy under a mutex).
*/
struct AnalysisResult {
	/** Whether the camera is currently open and delivering frames. */
	bool connected = false;
	/** Whether the position-tracking algorithm currently has a confident lock
	(always true in brightest-spot mode; false in color-blob mode when no
	pixels matched the target color this frame).
	*/
	bool found = false;

	/** 0..1, standard luminance-weighted brightness. */
	float brightness = 0.f;
	/** 0..1 average per-channel levels. */
	float r = 0.f;
	float g = 0.f;
	float b = 0.f;
	/** 0..1, frame-to-frame difference magnitude (post sensitivity gain, clamped). */
	float motion = 0.f;
	/** 0..1, normalized position within the frame. 0.5 = center.
	Holds its last known value when `found` is false. */
	float posX = 0.5f;
	float posY = 0.5f;

	/** Increments on every successfully analyzed frame; useful for staleness checks. */
	uint32_t frameCounter = 0;
};
