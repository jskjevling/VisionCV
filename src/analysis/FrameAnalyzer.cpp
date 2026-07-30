#include "FrameAnalyzer.hpp"
#include <opencv2/imgproc.hpp>
// OpenCV 5 moved image moments (cv::moments/cv::Moments) out of imgproc.hpp
// and into this header; older OpenCV (4.x) has it in imgproc.hpp itself, in
// which case this include is a harmless no-op via its own include guard.
#if __has_include(<opencv2/geometry/2d.hpp>)
#include <opencv2/geometry/2d.hpp>
#endif
#include <algorithm>
#include <cmath>


static void hueRangeMask(const cv::Mat& hsv, int hueCenter, int hueHalfWidth, cv::Mat& mask) {
	// OpenCV's 8-bit HSV hue channel is 0..179 (not 0..359) and wraps around.
	static const int H_MAX = 179;
	static const cv::Scalar SAT_VAL_LOW(0, 60, 60);
	static const cv::Scalar SAT_VAL_HIGH(0, 255, 255);

	int lo = hueCenter - hueHalfWidth;
	int hi = hueCenter + hueHalfWidth;

	if (lo < 0) {
		cv::Mat maskA, maskB;
		cv::inRange(hsv, cv::Scalar(0, 60, 60), cv::Scalar(hi, 255, 255), maskA);
		cv::inRange(hsv, cv::Scalar(H_MAX + 1 + lo, 60, 60), cv::Scalar(H_MAX, 255, 255), maskB);
		mask = maskA | maskB;
	}
	else if (hi > H_MAX) {
		cv::Mat maskA, maskB;
		cv::inRange(hsv, cv::Scalar(lo, 60, 60), cv::Scalar(H_MAX, 255, 255), maskA);
		cv::inRange(hsv, cv::Scalar(0, 60, 60), cv::Scalar(hi - H_MAX - 1, 255, 255), maskB);
		mask = maskA | maskB;
	}
	else {
		cv::inRange(hsv, cv::Scalar(lo, 60, 60), cv::Scalar(hi, 255, 255), mask);
	}
}


void FrameAnalyzer::reset() {
	prevGray.release();
	havePrev = false;
	lastPosX = 0.5f;
	lastPosY = 0.5f;
}


AnalysisResult FrameAnalyzer::analyze(const cv::Mat& frameRGB, const AnalysisParams& params) {
	AnalysisResult out;
	out.connected = true;

	cv::Mat gray;
	cv::cvtColor(frameRGB, gray, cv::COLOR_RGB2GRAY);

	// Per-channel + luminance-weighted brightness.
	cv::Scalar meanColor = cv::mean(frameRGB);
	out.r = static_cast<float>(meanColor[0] / 255.0);
	out.g = static_cast<float>(meanColor[1] / 255.0);
	out.b = static_cast<float>(meanColor[2] / 255.0);
	out.brightness = 0.299f * out.r + 0.587f * out.g + 0.114f * out.b;

	// Motion: mean absolute frame-to-frame luma difference, gained by the
	// sensitivity knob and clamped to 0..1.
	if (havePrev && prevGray.size() == gray.size()) {
		cv::Mat diff;
		cv::absdiff(gray, prevGray, diff);
		double meanDiff = cv::mean(diff)[0]; // 0..255
		float gain = 0.25f + params.motionSens * 4.0f;
		out.motion = std::min(1.f, static_cast<float>(meanDiff / 255.0) * gain);
	}
	prevGray = gray.clone();
	havePrev = true;

	// Position tracking.
	if (params.mode == TrackMode::BRIGHTEST_SPOT) {
		cv::Mat blurred;
		cv::GaussianBlur(gray, blurred, cv::Size(9, 9), 0);
		double minVal, maxVal;
		cv::Point minLoc, maxLoc;
		cv::minMaxLoc(blurred, &minVal, &maxVal, &minLoc, &maxLoc);
		out.found = true;
		lastPosX = gray.cols > 1 ? static_cast<float>(maxLoc.x) / (gray.cols - 1) : 0.5f;
		lastPosY = gray.rows > 1 ? static_cast<float>(maxLoc.y) / (gray.rows - 1) : 0.5f;
	}
	else {
		cv::Mat hsv, mask;
		cv::cvtColor(frameRGB, hsv, cv::COLOR_RGB2HSV);
		int hueCenter = static_cast<int>(params.targetHue * 179.f);
		int hueHalfWidth = std::max(2, static_cast<int>(params.tolerance * 45.f));
		hueRangeMask(hsv, hueCenter, hueHalfWidth, mask);

		cv::Moments m = cv::moments(mask, true);
		double minArea = 0.0005 * gray.cols * gray.rows; // ~0.05% of frame, filters noise
		if (m.m00 > minArea) {
			out.found = true;
			lastPosX = gray.cols > 1 ? static_cast<float>(m.m10 / m.m00) / (gray.cols - 1) : 0.5f;
			lastPosY = gray.rows > 1 ? static_cast<float>(m.m01 / m.m00) / (gray.rows - 1) : 0.5f;
		}
		else {
			out.found = false;
			// hold the last known lock rather than snapping to center
		}
	}
	out.posX = lastPosX;
	out.posY = lastPosY;

	return out;
}
