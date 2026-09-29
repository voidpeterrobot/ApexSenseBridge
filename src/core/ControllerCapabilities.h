#pragma once
#include "core/DeviceInfo.h"
#include <string_view>

namespace asb {
enum class ControllerModel { Unknown, Apex4, Apex5, Apex6Pro };
struct ControllerCapabilities {
    ControllerModel model = ControllerModel::Unknown;
    bool adaptiveTriggers = false;
    bool onboardProfiles = false;
    bool rawGripOutput = false;
};
// Direct-USB Apex6 vendor collection. Instance suffixes are discovered afresh.
inline bool isApex6Vendor(const HidDeviceInfo& d) {
    return d.vendorId == 0x37d7 && d.productId == 0x2502 &&
        d.usagePage == 0xffa0 && d.interfaceNumber == L"MI_02" &&
        d.inputReportLength == 33 && d.outputReportLength == 33 &&
        !d.containerId.empty() && !d.parentInstanceId.empty() &&
        d.instanceId.starts_with(L"HID\\VID_37D7&PID_2502&MI_02\\") &&
        d.parentInstanceId.starts_with(L"USB\\VID_37D7&PID_2502&MI_02\\");
}
inline ControllerCapabilities controllerCapabilities(const HidDeviceInfo& d) {
    if (isApex6Vendor(d)) return {ControllerModel::Apex6Pro, false, false, true};
    if(d.vendorId==0x04b4&&d.productId==0x2412&&d.usagePage==0xffa0)
        return {ControllerModel::Apex4,true,false,false};
    if(d.vendorId==0x37d7&&d.productId==0x2501&&d.usagePage==0xffa0)
        return {ControllerModel::Apex5,true,true,false};
    return {};
}
}
