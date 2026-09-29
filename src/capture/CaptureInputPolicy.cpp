#include "capture/CaptureInputPolicy.h"
#include <set>
namespace asb::capture {
std::optional<unsigned> selectCaptureXInputSlot(std::span<const HidDeviceInfo> devices,
    const HidDeviceInfo& selected,std::span<const unsigned> slots) {
    if(selected.vendorId!=0x37d7 || selected.productId!=0x2502 || selected.containerId.empty() ||
       slots.size()!=1 || slots[0]>3) return {};
    std::set<std::wstring> containers;
    bool found=false;
    for(const auto& d:devices) {
        if(d.vendorId!=selected.vendorId || d.productId!=selected.productId) continue;
        if(d.containerId.empty()) return {};
        containers.insert(d.containerId);
        found|=d.instanceId==selected.instanceId && d.containerId==selected.containerId;
    }
    if(!found || containers.size()!=1) return {};
    return slots[0];
}
dualsense::DualSenseInputState mapCaptureXInput(const platform::XInputSnapshot& raw) noexcept {
    auto state=platform::mapXInputState(raw);
    if(raw.leftY==0) state.ly=128;
    if(raw.rightY==0) state.ry=128;
    return state;
}
}
