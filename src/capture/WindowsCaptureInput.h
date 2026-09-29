#pragma once
#include "core/DeviceInfo.h"
#include "dualsense/DualSenseInput.h"
#include <memory>
#include <string>
#include <vector>
namespace asb::capture {
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
std::vector<HidDeviceInfo> captureInputDevices(std::string& error);
class WindowsCaptureInput {
public:
    WindowsCaptureInput();
    ~WindowsCaptureInput();
    bool open(const std::wstring& instance,std::string& error);
    // Timeout retains the last state; errors release the virtual source upstream.
    bool poll(dualsense::DualSenseInputState& state,std::string& error);
    std::string description() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
