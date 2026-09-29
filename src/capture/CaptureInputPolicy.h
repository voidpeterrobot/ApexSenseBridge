#pragma once
#include "core/DeviceInfo.h"
#include "platform/XInputMapping.h"
#include <optional>
#include <span>
#include <string>
namespace asb::capture {
std::optional<unsigned> selectCaptureXInputSlot(std::span<const HidDeviceInfo> devices,
    const HidDeviceInfo& selected,std::span<const unsigned> matchedSlots);
dualsense::DualSenseInputState mapCaptureXInput(const platform::XInputSnapshot& snapshot) noexcept;
}
