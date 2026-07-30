#pragma once
#include <opencv2/core.hpp>
#include "../shared/AnalysisResult.hpp"


enum class TrackMode {
	BRIGHTEST_SPOT = 0,
	COLOR_BLOB = 1,
};

/** Parameters read from the module's knobs/switch each frame.
Copied by value into the worker thread under a mutex, so this must stay a
plain-old-data struct. */
struct AnalysisParams {
	TrackMode mode = TrackMode::BRIGHTEST_SPOT;
	/** 0..1, normalized hue for color-blob target (maps to OpenCV's 0..179 H range). */
	float targetHue = 0.f;
	/** 0..1, width of the accepted hue band around targetHue. */
	float tolerance = 0.3f;
	/** 0..1, gain applied to the raw frame-difference motion signal. */
	float motionSens = 0.5f;
};

/** Runs the four image-analysis features (brightness, RGB levels, motion,
position tracking) on a single BGR/RGB frame. Stateful across calls (keeps
the previous frame for motion, and the last known position for hold-on-loss
behavior in color-blob mode), so one instance belongs to one camera stream.

Not thread-safe on its own — only ever called from the camera worker thread.
*/
class FrameAnalyzer {
public:
	/** frame must be a CV_8UC3 image in RGB channel order (which is what
	OpenPnP Capture's Cap_captureFrame always delivers, regardless of the
	camera's internal format). */
	AnalysisResult analyze(const cv::Mat& frameRGB, const AnalysisParams& params);

	/** Resets motion history and held position; call when switching cameras. */
	void reset();

private:
	cv::Mat prevGray;
	bool havePrev = false;
	float lastPosX = 0.5f;
	float lastPosY = 0.5f;
};
