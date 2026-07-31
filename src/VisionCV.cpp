#include "plugin.hpp"
#include "capture/CameraWorker.hpp"
#include <algorithm>
#include <cmath>


// Panel layout constants (mm). Used both to place widgets and (for the
// *_LABEL_Y ones) to draw matching text labels at runtime — see
// VisionCVWidget::draw(). Keep res/VisionCV.svg's comment in sync if these
// change. Panel is 14HP (71.12mm) wide.
namespace layout {
static const float PANEL_W = 71.12f;
static const float CENTER_X = PANEL_W / 2.f; // 35.56

static const float TITLE_Y = 7.f;

// Live preview thumbnail, above the camera-select display. No subtitle
// tagline below the title anymore — that space now just shortens the gap
// to the preview, and the whole content block below is pulled up by the
// same amount, growing the bottom margin instead.
static const float PREVIEW_W = 40.f;
static const float PREVIEW_H = 30.f; // 4:3
static const float PREVIEW_X = (PANEL_W - PREVIEW_W) / 2.f;
static const float PREVIEW_Y = 10.f;

static const float DISPLAY_X = 6.f;
static const float DISPLAY_Y = PREVIEW_Y + PREVIEW_H + 2.f; // 42
static const float DISPLAY_W = PANEL_W - 2.f * DISPLAY_X;
static const float DISPLAY_H = 7.f;

// Fixed size for the white output box: wide enough for the longest label and
// tall enough to span from just above the label down to just below the jack.
// Declared here (ahead of COL_LEFT_X/COL_RIGHT_X below, which reference
// OUT_BOX_W) rather than down by OutputBoxWidget where it's used, since
// namespace-scope const initializers must see earlier declarations only.
static const float OUT_BOX_W = 13.f;
static const float OUT_BOX_TOP_PAD = 2.5f; // above the label baseline
static const float OUT_BOX_BOTTOM_PAD = 5.f; // below the port center (radius + margin)

// Shared 3-column grid used by both the knob rows and the jack rows, so
// everything lines up vertically across the whole panel. The outer columns
// are set so the output boxes' own outer edges land exactly on the camera
// display's edges (DISPLAY_X and DISPLAY_X + DISPLAY_W) — the boxes and the
// display line up cleanly instead of one being a bit wider than the other.
static const float COL_LEFT_X = DISPLAY_X + OUT_BOX_W / 2.f;
static const float COL_CENTER_X = CENTER_X;
static const float COL_RIGHT_X = DISPLAY_X + DISPLAY_W - OUT_BOX_W / 2.f;

// Knob rows: a RoundBlackKnob is ~10mm across (5mm radius), so a label
// needs to sit ~6mm above its own row's control center to clear it, and
// 15mm of row pitch clears the row above's knob too.
static const float ROW1_LABEL_Y = 53.f; // mode / hue / freeze
static const float ROW1_Y = 59.f;
static const float ROW2_LABEL_Y = 68.f; // tolerance / motion sens / smoothing
static const float ROW2_Y = 74.f;

static const float DIVIDER_Y = 81.f;

// 9 jacks total (1 input + 8 outputs, including the FOUND gate) as 3 rows
// of 3 instead of 2 rows of 4, matching the knob columns above. Outputs are
// marked with an inverted (white box, black text) label that wraps the jack
// as well as the text — see OutputBoxWidget.
static const float JACK_ROW1_LABEL_Y = 86.f; // freeze / red / green
static const float JACK_ROW1_Y = 91.f;
static const float JACK_ROW2_LABEL_Y = 99.f; // blue / bright / motion
static const float JACK_ROW2_Y = 104.f;
static const float JACK_ROW3_LABEL_Y = 112.f; // pos x / pos y / found
static const float JACK_ROW3_Y = 117.f;
} // namespace layout


struct VisionCV : Module {
	enum ParamId {
		MODE_PARAM,
		HUE_PARAM,
		TOLERANCE_PARAM,
		MOTION_SENS_PARAM,
		SMOOTHING_PARAM,
		FREEZE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		FREEZE_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		BRIGHTNESS_OUTPUT,
		RED_OUTPUT,
		GREEN_OUTPUT,
		BLUE_OUTPUT,
		MOTION_OUTPUT,
		POS_X_OUTPUT,
		POS_Y_OUTPUT,
		FOUND_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		FREEZE_LIGHT,
		LIGHTS_LEN
	};

	CameraWorker camera;

	dsp::SchmittTrigger freezeButtonTrigger;
	dsp::SchmittTrigger freezeCvTrigger;
	bool frozen = false;

	dsp::ExponentialFilter brightnessFilter;
	dsp::ExponentialFilter rFilter;
	dsp::ExponentialFilter gFilter;
	dsp::ExponentialFilter bFilter;
	dsp::ExponentialFilter motionFilter;
	dsp::ExponentialFilter posXFilter;
	dsp::ExponentialFilter posYFilter;

	VisionCV() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Tracking mode", {"Brightest spot", "Color blob"});
		configParam(HUE_PARAM, 0.f, 1.f, 0.f, "Target hue");
		configParam(TOLERANCE_PARAM, 0.f, 1.f, 0.3f, "Tolerance");
		configParam(MOTION_SENS_PARAM, 0.f, 1.f, 0.5f, "Motion sensitivity");
		configParam(SMOOTHING_PARAM, 0.f, 1.f, 0.3f, "Smoothing");
		configButton(FREEZE_PARAM, "Freeze");
		configLight(FREEZE_LIGHT, "Freeze active");
		configInput(FREEZE_CV_INPUT, "Freeze trigger");
		configOutput(BRIGHTNESS_OUTPUT, "Brightness");
		configOutput(RED_OUTPUT, "Red level");
		configOutput(GREEN_OUTPUT, "Green level");
		configOutput(BLUE_OUTPUT, "Blue level");
		configOutput(MOTION_OUTPUT, "Motion");
		configOutput(POS_X_OUTPUT, "Position X");
		configOutput(POS_Y_OUTPUT, "Position Y");
		configOutput(FOUND_OUTPUT, "Found (color-lock gate)");
	}

	void selectCamera(int index) {
		camera.selectDevice(index);
	}

	void process(const ProcessArgs& args) override {
		// Freeze toggling always runs, even while frozen, so it can be un-frozen.
		if (freezeButtonTrigger.process(params[FREEZE_PARAM].getValue() > 0.f))
			frozen = !frozen;
		if (freezeCvTrigger.process(inputs[FREEZE_CV_INPUT].getVoltage()))
			frozen = !frozen;
		lights[FREEZE_LIGHT].setBrightness(frozen ? 1.f : 0.f);

		// Push current knob/switch state to the worker thread (cheap: a small
		// struct copy under a mutex).
		AnalysisParams p;
		p.mode = (params[MODE_PARAM].getValue() > 0.5f) ? TrackMode::COLOR_BLOB : TrackMode::BRIGHTEST_SPOT;
		p.targetHue = params[HUE_PARAM].getValue();
		p.tolerance = params[TOLERANCE_PARAM].getValue();
		p.motionSens = params[MOTION_SENS_PARAM].getValue();
		camera.setAnalysisParams(p);

		if (frozen)
			return;

		AnalysisResult result = camera.getLatestResult();

		float smoothing = params[SMOOTHING_PARAM].getValue();
		float tau = 0.001f + smoothing * smoothing * 0.4f;
		brightnessFilter.setTau(tau);
		rFilter.setTau(tau);
		gFilter.setTau(tau);
		bFilter.setTau(tau);
		motionFilter.setTau(tau);
		posXFilter.setTau(tau);
		posYFilter.setTau(tau);

		float brightness = brightnessFilter.process(args.sampleTime, result.brightness);
		float r = rFilter.process(args.sampleTime, result.r);
		float g = gFilter.process(args.sampleTime, result.g);
		float b = bFilter.process(args.sampleTime, result.b);
		float motion = motionFilter.process(args.sampleTime, result.motion);
		float posX = posXFilter.process(args.sampleTime, result.posX);
		float posY = posYFilter.process(args.sampleTime, result.posY);

		outputs[BRIGHTNESS_OUTPUT].setVoltage(clamp(brightness, 0.f, 1.f) * 10.f);
		outputs[RED_OUTPUT].setVoltage(clamp(r, 0.f, 1.f) * 10.f);
		outputs[GREEN_OUTPUT].setVoltage(clamp(g, 0.f, 1.f) * 10.f);
		outputs[BLUE_OUTPUT].setVoltage(clamp(b, 0.f, 1.f) * 10.f);
		outputs[MOTION_OUTPUT].setVoltage(clamp(motion, 0.f, 1.f) * 10.f);
		// 0..1 -> -5V..+5V, 0.5 = center. Invert Y so "up" in frame reads positive.
		outputs[POS_X_OUTPUT].setVoltage(clamp(posX, 0.f, 1.f) * 10.f - 5.f);
		outputs[POS_Y_OUTPUT].setVoltage(-(clamp(posY, 0.f, 1.f) * 10.f - 5.f));
		// High whenever the tracker has a confident lock: always true in
		// brightest-spot mode, and in color-blob mode only when this frame's
		// pixels actually matched the target color band.
		outputs[FOUND_OUTPUT].setVoltage(result.found ? 10.f : 0.f);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		std::string uid = camera.getDeviceUniqueId();
		if (!uid.empty())
			json_object_set_new(rootJ, "cameraUniqueId", json_string(uid.c_str()));
		std::string name = camera.getDeviceName();
		if (!name.empty())
			json_object_set_new(rootJ, "cameraName", json_string(name.c_str()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* uidJ = json_object_get(rootJ, "cameraUniqueId");
		if (uidJ) {
			// Non-blocking: just posts the request. The worker thread
			// resolves it against its own device cache once populated (and
			// retries internally until it is), so a single call here is
			// enough even if the device isn't enumerable yet (e.g. USB not
			// settled at patch-load time). Silently does nothing if no
			// matching device ever turns up.
			camera.selectDeviceByUniqueId(json_string_value(uidJ));
		}
	}
};


/** Populates a menu with one checkable item per known camera device. Shared
between the right-click context menu and the on-panel camera display's
click menu, so both stay in sync. */
static void populateCameraMenu(Menu* menu, VisionCV* module) {
	menu->addChild(createMenuLabel("Camera"));

	std::vector<CameraWorker::DeviceInfo> devices = module->camera.listDevices();
	if (devices.empty()) {
		menu->addChild(createMenuLabel("(no cameras found yet)"));
	}
	else {
		// Compare by unique ID (not index) so the checkmark is correct
		// whether the camera was chosen from this menu or restored from a
		// saved patch.
		std::string currentUid = module->camera.getDeviceUniqueId();
		for (size_t i = 0; i < devices.size(); i++) {
			std::string name = devices[i].name;
			std::string uid = devices[i].uniqueId;
			menu->addChild(createCheckMenuItem(name, "",
				[=]() { return !uid.empty() && uid == currentUid; },
				[=]() { module->selectCamera((int) i); }
			));
		}
	}
}


/** Standard RGB (0..255 each) -> normalized hue (0..1), matching how
FrameAnalyzer maps VisionCV::HUE_PARAM (0..1) to OpenCV's 0..179 hue range
(targetHue * 179 there == hueDegrees/360 here, near enough — OpenCV's 8-bit
hue is degrees/2, i.e. 0..179 instead of 0..358, a <1% difference from 360
that doesn't matter for picking a color band). */
static float rgbToHueNormalized(uint8_t r8, uint8_t g8, uint8_t b8) {
	float r = r8 / 255.f, g = g8 / 255.f, b = b8 / 255.f;
	float maxC = std::max(r, std::max(g, b));
	float minC = std::min(r, std::min(g, b));
	float delta = maxC - minC;
	float hueDeg = 0.f;
	if (delta > 1e-6f) {
		if (maxC == r)
			hueDeg = 60.f * std::fmod(((g - b) / delta), 6.f);
		else if (maxC == g)
			hueDeg = 60.f * (((b - r) / delta) + 2.f);
		else
			hueDeg = 60.f * (((r - g) / delta) + 4.f);
	}
	if (hueDeg < 0.f)
		hueDeg += 360.f;
	return hueDeg / 360.f;
}


/** On-panel clickable display showing the selected camera (or a prompt to
pick one) — click it to open the same camera list as the right-click menu.
This is the discoverable equivalent of the "device selector" widgets Core's
Audio/MIDI modules use.

Draws its own background/text (via NanoVG) rather than subclassing
LedDisplayChoice: that widget's internal text vertical-centering assumes a
particular cell height, and since this display isn't sized to match that
assumption, its text ended up shifted/clipped. Self-drawing gives full
control that scales correctly with whatever box.size this ends up with. */
struct CameraDisplay : widget::OpaqueWidget {
	VisionCV* module = nullptr;
	std::shared_ptr<Font> font;
	std::string text = "Camera: click to select";

	void step() override {
		if (module) {
			bool connected = module->camera.isConnected();
			std::string name = module->camera.getDeviceName();
			text = !name.empty()
				? std::string(connected ? "\xE2\x97\x8F " : "\xE2\x97\x8B ") + name // ●/○
				: "Camera: click to select";
		}
		OpaqueWidget::step();
	}

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, nvgRGB(0x12, 0x13, 0x16));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x3a, 0x3c, 0x42));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);

		if (!font)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font && font->handle >= 0) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, box.size.y * 0.55f);
			nvgFillColor(args.vg, nvgRGB(0xc7, 0xc9, 0xcf));
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgText(args.vg, 6.f, box.size.y * 0.5f, text.c_str(), NULL);
		}

		OpaqueWidget::draw(args);
	}

	void onButton(const ButtonEvent& e) override {
		OpaqueWidget::onButton(e);
		// Left-click only: right-click still falls through to Rack's normal
		// module context menu (which also has the Camera section).
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			e.consume(this);
			Menu* menu = createMenu();
			populateCameraMenu(menu, module);
		}
	}
};


/** Live camera preview, letterboxed to preserve the source aspect ratio.
Also replaces the old dedicated status light: a blank/dark box already
reads as "nothing coming in" without a separate indicator. Click it (in
color-blob mode, or to switch into it) to sample a pixel as the new target
hue — the discoverable equivalent of a color picker. */
struct CameraPreviewWidget : widget::OpaqueWidget {
	VisionCV* module = nullptr;

	int imageHandle = -1;
	int imageWidth = 0;
	int imageHeight = 0;
	uint32_t lastFrameCounter = 0;
	std::vector<uint8_t> lastRgba;

	float fitX = 0.f, fitY = 0.f, fitW = 0.f, fitH = 0.f;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(0x0a, 0x0a, 0x0c));
		nvgFill(args.vg);

		if (module) {
			Thumbnail thumb = module->camera.getLatestThumbnail();
			if (thumb.width > 0 && thumb.height > 0 && !thumb.rgba.empty()) {
				if (imageHandle < 0 || imageWidth != thumb.width || imageHeight != thumb.height) {
					if (imageHandle >= 0)
						nvgDeleteImage(args.vg, imageHandle);
					imageHandle = nvgCreateImageRGBA(args.vg, thumb.width, thumb.height, 0, thumb.rgba.data());
					imageWidth = thumb.width;
					imageHeight = thumb.height;
				}
				else if (thumb.frameCounter != lastFrameCounter) {
					nvgUpdateImage(args.vg, imageHandle, thumb.rgba.data());
				}
				lastFrameCounter = thumb.frameCounter;
				lastRgba = thumb.rgba;

				if (imageHandle >= 0) {
					float srcAspect = static_cast<float>(thumb.width) / thumb.height;
					float dstAspect = box.size.x / box.size.y;
					if (srcAspect > dstAspect) {
						fitW = box.size.x;
						fitH = fitW / srcAspect;
						fitX = 0.f;
						fitY = (box.size.y - fitH) / 2.f;
					}
					else {
						fitH = box.size.y;
						fitW = fitH * srcAspect;
						fitY = 0.f;
						fitX = (box.size.x - fitW) / 2.f;
					}
					NVGpaint paint = nvgImagePattern(args.vg, fitX, fitY, fitW, fitH, 0.f, imageHandle, 1.f);
					nvgBeginPath(args.vg);
					nvgRect(args.vg, fitX, fitY, fitW, fitH);
					nvgFillPaint(args.vg, paint);
					nvgFill(args.vg);
				}
			}
		}

		nvgStrokeColor(args.vg, nvgRGB(0x3a, 0x3c, 0x42));
		nvgStrokeWidth(args.vg, 1.f);
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f);
		nvgStroke(args.vg);

		OpaqueWidget::draw(args);
	}

	void onButton(const ButtonEvent& e) override {
		OpaqueWidget::onButton(e);
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT
			&& !lastRgba.empty() && fitW > 0.f && fitH > 0.f) {
			Vec local = e.pos;
			if (local.x >= fitX && local.x < fitX + fitW && local.y >= fitY && local.y < fitY + fitH) {
				float u = (local.x - fitX) / fitW;
				float v = (local.y - fitY) / fitH;
				int px = std::min(imageWidth - 1, std::max(0, static_cast<int>(u * imageWidth)));
				int py = std::min(imageHeight - 1, std::max(0, static_cast<int>(v * imageHeight)));
				size_t idx = (static_cast<size_t>(py) * imageWidth + px) * 4;
				if (idx + 2 < lastRgba.size()) {
					float hue = rgbToHueNormalized(lastRgba[idx], lastRgba[idx + 1], lastRgba[idx + 2]);
					module->params[VisionCV::HUE_PARAM].setValue(hue);
					// Picking a color only makes sense in color-blob mode.
					module->params[VisionCV::MODE_PARAM].setValue(1.f);
				}
				e.consume(this);
			}
		}
	}
};


/** Plain white rounded-rect background, sized/positioned in the widget
constructor to span both an output's label and its jack — added as a child
*before* the corresponding port, so it draws underneath the port (and the
label text, drawn afterward in VisionCVWidget::draw()) instead of covering
them. Mirrors the "boxed output" convention some VCV modules use. */
struct OutputBoxWidget : widget::Widget {
	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, mm2px(0.8f));
		nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
		nvgFill(args.vg);
	}
};


struct VisionCVWidget : ModuleWidget {
	std::shared_ptr<Font> titleFont;
	std::shared_ptr<Font> bodyFont;

	VisionCVWidget(VisionCV* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VisionCV.svg")));
		box.size = Vec(RACK_GRID_WIDTH * 14, RACK_GRID_HEIGHT);

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace layout;

		CameraPreviewWidget* preview = createWidget<CameraPreviewWidget>(mm2px(Vec(PREVIEW_X, PREVIEW_Y)));
		preview->box.size = mm2px(Vec(PREVIEW_W, PREVIEW_H));
		preview->module = module;
		addChild(preview);

		CameraDisplay* cameraDisplay = createWidget<CameraDisplay>(mm2px(Vec(DISPLAY_X, DISPLAY_Y)));
		cameraDisplay->box.size = mm2px(Vec(DISPLAY_W, DISPLAY_H));
		cameraDisplay->module = module;
		addChild(cameraDisplay);

		auto addOutputBox = [&](float xmm, float labelYmm, float portYmm) {
			float topMm = labelYmm - OUT_BOX_TOP_PAD;
			float bottomMm = portYmm + OUT_BOX_BOTTOM_PAD;
			OutputBoxWidget* out = createWidget<OutputBoxWidget>(mm2px(Vec(xmm - OUT_BOX_W / 2.f, topMm)));
			out->box.size = mm2px(Vec(OUT_BOX_W, bottomMm - topMm));
			addChild(out);
		};

		// Knob rows: 3 across, 2 rows.
		addParam(createParamCentered<CKSS>(mm2px(Vec(COL_LEFT_X, ROW1_Y)), module, VisionCV::MODE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(COL_CENTER_X, ROW1_Y)), module, VisionCV::HUE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(COL_RIGHT_X, ROW1_Y)), module, VisionCV::TOLERANCE_PARAM));

		addParam(createLightParamCentered<VCVLightButton<GreenLight>>(mm2px(Vec(COL_LEFT_X, ROW2_Y)), module, VisionCV::FREEZE_PARAM, VisionCV::FREEZE_LIGHT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(COL_CENTER_X, ROW2_Y)), module, VisionCV::MOTION_SENS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(COL_RIGHT_X, ROW2_Y)), module, VisionCV::SMOOTHING_PARAM));

		// Jack rows: 3 across, 3 rows (1 input + 8 outputs). Freeze input is
		// plain (not boxed); everything else here is an output and gets a box.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(COL_LEFT_X, JACK_ROW1_Y)), module, VisionCV::FREEZE_CV_INPUT));
		addOutputBox(COL_CENTER_X, JACK_ROW1_LABEL_Y, JACK_ROW1_Y);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL_CENTER_X, JACK_ROW1_Y)), module, VisionCV::FOUND_OUTPUT));
		addOutputBox(COL_RIGHT_X, JACK_ROW1_LABEL_Y, JACK_ROW1_Y);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL_RIGHT_X, JACK_ROW1_Y)), module, VisionCV::BRIGHTNESS_OUTPUT));

		addOutputBox(COL_LEFT_X, JACK_ROW2_LABEL_Y, JACK_ROW2_Y);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL_LEFT_X, JACK_ROW2_Y)), module, VisionCV::RED_OUTPUT));
		addOutputBox(COL_CENTER_X, JACK_ROW2_LABEL_Y, JACK_ROW2_Y);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL_CENTER_X, JACK_ROW2_Y)), module, VisionCV::GREEN_OUTPUT));
		addOutputBox(COL_RIGHT_X, JACK_ROW2_LABEL_Y, JACK_ROW2_Y);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL_RIGHT_X, JACK_ROW2_Y)), module, VisionCV::BLUE_OUTPUT));

		addOutputBox(COL_LEFT_X, JACK_ROW3_LABEL_Y, JACK_ROW3_Y);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL_LEFT_X, JACK_ROW3_Y)), module, VisionCV::MOTION_OUTPUT));
		addOutputBox(COL_CENTER_X, JACK_ROW3_LABEL_Y, JACK_ROW3_Y);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL_CENTER_X, JACK_ROW3_Y)), module, VisionCV::POS_X_OUTPUT));
		addOutputBox(COL_RIGHT_X, JACK_ROW3_LABEL_Y, JACK_ROW3_Y);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL_RIGHT_X, JACK_ROW3_Y)), module, VisionCV::POS_Y_OUTPUT));
	}

	/** Panel text is drawn here, not baked into the SVG, because VCV Rack's
	SVG panels are rendered via NanoSVG, which does not render <text>
	elements as real glyphs. NanoVG's nvgText() (used here) does support
	text properly and doesn't require any SVG-authoring-tool workaround. */
	void draw(const DrawArgs& args) override {
		ModuleWidget::draw(args);

		if (!titleFont)
			titleFont = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!bodyFont)
			bodyFont = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (!titleFont || titleFont->handle < 0 || !bodyFont || bodyFont->handle < 0)
			return;

		using namespace layout;

		NVGcolor titleColor = nvgRGB(0x5f, 0xd0, 0xe8);
		NVGcolor labelColor = nvgRGB(0xc7, 0xc9, 0xcf);

		nvgSave(args.vg);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

		auto label = [&](Font* f, float xmm, float ymm, float sizeMm, NVGcolor color, const char* str) {
			Vec pos = mm2px(Vec(xmm, ymm));
			nvgFontFaceId(args.vg, f->handle);
			nvgFontSize(args.vg, mm2px(sizeMm));
			nvgFillColor(args.vg, color);
			nvgText(args.vg, pos.x, pos.y, str, NULL);
		};

		// Outputs get black text (instead of the usual light-gray label
		// color) since it now sits on the white OutputBoxWidget background
		// that's already been drawn behind the jack — see the constructor.
		auto outputLabel = [&](float xmm, float ymm, float sizeMm, const char* str) {
			label(bodyFont.get(), xmm, ymm, sizeMm, nvgRGB(0x10, 0x10, 0x12), str);
		};

		// "VisionCV" with the interior letters set smaller, like a small-caps
		// wordmark: draws "V" / "ISION" / "CV" as separate segments at two
		// sizes, measuring each with nvgTextBounds so the whole assembly
		// still ends up centered on CENTER_X.
		auto smallCapsTitle = [&](float xmm, float ymm, float bigMm, float smallMm, NVGcolor color) {
			struct Seg { const char* text; float sizeMm; };
			Seg segs[] = {{"V", bigMm}, {"ISION", smallMm}, {"CV", bigMm}};

			nvgFontFaceId(args.vg, titleFont->handle);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
			nvgFillColor(args.vg, color);

			float totalWidth = 0.f;
			float bounds[4];
			for (Seg& seg : segs) {
				nvgFontSize(args.vg, mm2px(seg.sizeMm));
				nvgTextBounds(args.vg, 0.f, 0.f, seg.text, NULL, bounds);
				totalWidth += bounds[2] - bounds[0];
			}

			float x = mm2px(xmm) - totalWidth / 2.f;
			float y = mm2px(ymm);
			for (Seg& seg : segs) {
				nvgFontSize(args.vg, mm2px(seg.sizeMm));
				nvgText(args.vg, x, y, seg.text, NULL);
				nvgTextBounds(args.vg, 0.f, 0.f, seg.text, NULL, bounds);
				x += bounds[2] - bounds[0];
			}
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
		};

		// Title/branding in Nunito Bold (VCV Rack's own UI font); all
		// control/jack labels in DejaVu Sans, all-caps, matching the
		// small-caps-label convention used across VCV's stock modules.
		smallCapsTitle(CENTER_X, TITLE_Y, 6.f, 4.3f, titleColor);

		label(bodyFont.get(), COL_LEFT_X, ROW1_LABEL_Y, 3.f, labelColor, "MODE");
		label(bodyFont.get(), COL_CENTER_X, ROW1_LABEL_Y, 3.f, labelColor, "HUE");
		label(bodyFont.get(), COL_RIGHT_X, ROW1_LABEL_Y, 3.f, labelColor, "TOLERANCE");

		label(bodyFont.get(), COL_LEFT_X, ROW2_LABEL_Y, 3.f, labelColor, "FREEZE");
		label(bodyFont.get(), COL_CENTER_X, ROW2_LABEL_Y, 3.f, labelColor, "MOTION SENS");
		label(bodyFont.get(), COL_RIGHT_X, ROW2_LABEL_Y, 3.f, labelColor, "SMOOTHING");

		label(bodyFont.get(), COL_LEFT_X, JACK_ROW1_LABEL_Y, 3.f, labelColor, "FREEZE");
		outputLabel(COL_CENTER_X, JACK_ROW1_LABEL_Y, 3.f, "FOUND");
		outputLabel(COL_RIGHT_X, JACK_ROW1_LABEL_Y, 3.f, "BRIGHT");

		outputLabel(COL_LEFT_X, JACK_ROW2_LABEL_Y, 3.f, "RED");
		outputLabel(COL_CENTER_X, JACK_ROW2_LABEL_Y, 3.f, "GREEN");
		outputLabel(COL_RIGHT_X, JACK_ROW2_LABEL_Y, 3.f, "BLUE");

		outputLabel(COL_LEFT_X, JACK_ROW3_LABEL_Y, 3.f, "MOTION");
		outputLabel(COL_CENTER_X, JACK_ROW3_LABEL_Y, 3.f, "POS X");
		outputLabel(COL_RIGHT_X, JACK_ROW3_LABEL_Y, 3.f, "POS Y");

		nvgRestore(args.vg);
	}

	void appendContextMenu(Menu* menu) override {
		VisionCV* module = getModule<VisionCV>();
		if (!module)
			return;

		menu->addChild(new MenuSeparator);
		populateCameraMenu(menu, module);
	}
};


Model* modelVisionCV = createModel<VisionCV, VisionCVWidget>("VisionCV");
