#pragma once
#include "dualsense/VirtualDualSense.h"
#include "dualsense/TouchpadGestureProfile.h"
#include "core/DeviceInfo.h"
#include "platform/SessionControl.h"
#include <chrono>
#include <optional>

namespace asb::cli {
struct BridgeCommandOptions {
    std::optional<std::size_t> deviceIndex;
    std::optional<std::chrono::seconds> duration;
    std::filesystem::path viiperExecutable;
    asb::dualsense::VirtualDualSenseBackend virtualBackend = asb::dualsense::VirtualDualSenseBackend::Auto;
    bool proxyXInput=true,routeRumble=false,verifyVirtualInput=false,isolateApex=true;
    asb::dualsense::TouchpadGestureProfile touchpadProfile=asb::dualsense::TouchpadGestureProfile::None;
    bool touchpadProfileExplicit=false;
    unsigned int hapticThresholdPercent=12;
    bool hapticThresholdExplicit=false;
    std::optional<unsigned int> xinputIndex;
    std::optional<std::string> sessionToken;
    std::optional<std::uint8_t> apexProfileSlot;
    std::filesystem::path telemetryJson;
    bool apex6Consent=false;
    bool requireApex6=false;
    bool apex6DongleDiagnostic=false;
    bool apex6DongleBeta=false;
    std::optional<double> gripGain;
};
#ifdef _WIN32
int commandApex6Bridge(const HidDeviceInfo&,const BridgeCommandOptions&,
    platform::SessionControl*,platform::GlobalSessionStop&);
#endif
}
