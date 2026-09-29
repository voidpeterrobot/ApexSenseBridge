#pragma once
#include "core/DeviceInfo.h"
#include <string>
#include <vector>
namespace asb::platform {
// Enumeration uses metadata-only handles (desired access 0). This module has
// no physical output transport factory or effect submission implementation.
[[nodiscard]] std::vector<HidDeviceInfo> enumerateHidDevices(std::string& error);
}
