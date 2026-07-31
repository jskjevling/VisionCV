#pragma once
#include <string>
#include <cstdint>


struct CameraDeviceInfo {
	std::string name;
	std::string uniqueId;
};

struct CameraFormatOption {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps = 0;
};
