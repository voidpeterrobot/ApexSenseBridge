#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>
#include <Xinput.h>
#include "capture/WindowsCaptureInput.h"
#include "capture/CaptureInputPolicy.h"
#include "platform/HidDiscovery.h"
#include "platform/PhysicalInputSource.h"
#include "platform/XInputMapping.h"
#include <algorithm>
#include <atomic>
#include <set>
#include <stdexcept>

namespace asb::capture {
std::string utf8(const std::wstring& value) {
    if(value.empty()) return {};
    const auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(!n) throw std::runtime_error("invalid UTF-16 identifier");
    std::string out(n,'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n,nullptr,nullptr); return out;
}
std::wstring wide(const std::string& value) {
    if(value.empty()) return {};
    const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(!n) throw std::runtime_error("invalid UTF-8 identifier");
    std::wstring out(n,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n); return out;
}
std::vector<HidDeviceInfo> captureInputDevices(std::string& error) {
    auto devices=platform::enumerateHidDevices(error);
    std::erase_if(devices,[](const auto& d){return d.vendorId!=0x37d7 || d.productId!=0x2502 || d.usagePage!=1 || d.usage!=5;});
    return devices;
}
namespace {
struct CapabilitiesEx { XINPUT_CAPABILITIES capabilities{}; WORD vendor{},product{},version{},unknown1{}; DWORD unknown2{}; };
using Query=DWORD(WINAPI*)(DWORD,DWORD,DWORD,CapabilitiesEx*);
std::vector<unsigned> matchingSlots(Query query) {
    std::vector<unsigned> matches;
    for(DWORD i=0;i<4;++i) {
        XINPUT_STATE state{}; if(XInputGetState(i,&state)!=ERROR_SUCCESS) continue;
        CapabilitiesEx cap{}; auto result=query(1,i,0,&cap);
        if(result!=ERROR_SUCCESS) { cap={}; result=query(0,i,0,&cap); }
        if(result==ERROR_SUCCESS && cap.vendor==0x37d7 && cap.product==0x2502) matches.push_back(i);
    }
    return matches;
}
}
struct WindowsCaptureInput::Impl {
    HidDeviceInfo info;
    std::unique_ptr<platform::PhysicalInputSource> hid;
    HCMNOTIFICATION notification=nullptr;
    HMODULE library=nullptr; Query query=nullptr; DWORD slot=0;
    std::atomic_bool changed{false};
    bool opened=false;
    ~Impl() { if(notification) CM_Unregister_Notification(notification); if(library) FreeLibrary(library); }
    static DWORD CALLBACK notify(HCMNOTIFICATION,PVOID context,CM_NOTIFY_ACTION action,PCM_NOTIFY_EVENT_DATA,DWORD) {
        if(action==CM_NOTIFY_ACTION_DEVICEINSTANCEENUMERATED || action==CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED ||
           action==CM_NOTIFY_ACTION_DEVICEINSTANCESTARTED) static_cast<Impl*>(context)->changed=true;
        return ERROR_SUCCESS;
    }
};
WindowsCaptureInput::WindowsCaptureInput():impl_(std::make_unique<Impl>()) {}
WindowsCaptureInput::~WindowsCaptureInput()=default;
bool WindowsCaptureInput::open(const std::wstring& instance,std::string& error) {
    auto& s=*impl_;
    if(s.opened || s.notification) { error="input source is single-use"; return false; }
    auto devices=captureInputDevices(error); if(!error.empty()) return false;
    const auto selected=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.instanceId==instance;});
    if(selected==devices.end() || selected->containerId.empty()) { error="exact Apex6Pro gamepad instance/container required"; return false; }
    s.info=*selected;
    CM_NOTIFY_FILTER filter{}; filter.cbSize=sizeof(filter); filter.FilterType=CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
    if(instance.size()>=MAX_DEVICE_ID_LEN) { error="device instance identifier too long"; return false; }
    std::copy(instance.begin(),instance.end(),filter.u.DeviceInstance.InstanceId);
    if(CM_Register_Notification(&filter,&s,&Impl::notify,&s.notification)!=CR_SUCCESS) {
        error="cannot monitor selected device removal; refusing unguarded input"; return false;
    }
    std::string hidError;
    s.hid=platform::openGenericHidInputSource(s.info,hidError);
    if(!s.hid) {
        // XInput does not expose a container ID. Accept only independently unique
        // VID/PID evidence on BOTH sides; never choose the sole unmatched pad.
        std::set<std::wstring> containers;
        for(const auto& d:devices) containers.insert(d.containerId);
        if(containers.size()!=1 || containers.contains(L"")) { error="ambiguous Apex6Pro containers; XInput association refused"; return false; }
        s.library=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(s.library) s.query=reinterpret_cast<Query>(GetProcAddress(s.library,MAKEINTRESOURCEA(108)));
        if(!s.query) { error=hidError+"; XInput identity query unavailable"; return false; }
        const auto slots=matchingSlots(s.query);
        const auto selectedSlot=selectCaptureXInputSlot(devices,s.info,slots);
        if(!selectedSlot) { error=hidError+"; no unique VID/PID-matched XInput slot (no unchecked fallback)"; return false; }
        s.slot=*selectedSlot;
    }
    // Re-enumerate after establishing removal monitoring, closing the selection race.
    devices=captureInputDevices(error);
    if(!error.empty() || s.changed || std::none_of(devices.begin(),devices.end(),[&](const auto& d){
        return d.instanceId==s.info.instanceId && d.containerId==s.info.containerId && d.path==s.info.path;
    })) { error="selected input changed during opening"; return false; }
    s.opened=true; error.clear(); return true;
}
bool WindowsCaptureInput::poll(dualsense::DualSenseInputState& state,std::string& error) {
    auto& s=*impl_;
    if(!s.opened || s.changed) { error="selected input removed or replaced; no automatic reassociation"; state={}; return false; }
    if(s.hid) {
        const auto status=s.hid->waitForState(state,std::chrono::milliseconds(4),error);
        if(status==platform::PhysicalInputStatus::Error || status==platform::PhysicalInputStatus::Disconnected) { state={}; return false; }
        return true;
    }
    const auto slots=matchingSlots(s.query);
    XINPUT_STATE raw{};
    if(slots.size()!=1 || slots.front()!=s.slot || XInputGetState(s.slot,&raw)!=ERROR_SUCCESS) {
        state={}; error="selected XInput association lost/ambiguous"; return false;
    }
    const auto& pad=raw.Gamepad;
    state=mapCaptureXInput({pad.sThumbLX,pad.sThumbLY,pad.sThumbRX,pad.sThumbRY,pad.bLeftTrigger,pad.bRightTrigger,pad.wButtons});
    return true;
}
std::string WindowsCaptureInput::description() const {
    const auto& s=*impl_;
    return "apex6pro-input-only;instance="+utf8(s.info.instanceId)+";container="+utf8(s.info.containerId)+
        ";api="+(s.hid?std::string(s.hid->backendName()):"xinput-vidpid-unique-slot-"+std::to_string(s.slot));
}
}
