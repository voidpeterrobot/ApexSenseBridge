#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include "capture/WindowsCaptureBackend.h"
#include "capture/RawCapture.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <array>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace asb::capture {
namespace {
using Handle = std::uintptr_t;
using Bool = std::uint8_t;
using Callback = void(__cdecl*)(Handle,const std::uint8_t*,std::uint32_t);
struct ServerConfig { char* address; std::uint64_t connectionMs, handlerMs; std::uint32_t flushMs; };
struct DeviceMeta {
    const char* serial; const char* mac; const char* board;
    std::uint8_t battery; double temperature; double voltage; const char* color;
};
template<class T> void symbol(HMODULE module, const char* name, T& function) {
    function=reinterpret_cast<T>(GetProcAddress(module,name));
    if(!function) throw std::runtime_error(std::string("Capture requires a matching raw-capable libVIIPER: missing ")+name);
}
}
struct WindowsCaptureBackend::Impl {
    explicit Impl(RawFeedbackSink& r):recorder(r) {}
    RawFeedbackSink& recorder;
    HMODULE module=nullptr;
    Handle server=0,device=0; std::uint32_t bus=0;
    std::atomic_bool closing{false}, lost{false}, hostConnected{false};
    std::string serial,mac;
    std::uint32_t(__cdecl* capabilities)(std::uint32_t)=nullptr;
    Bool(__cdecl* newServer)(ServerConfig*,Handle*,void*)=nullptr;
    Bool(__cdecl* closeServer)(Handle)=nullptr;
    Bool(__cdecl* createBus)(Handle,std::uint32_t*)=nullptr;
    Bool(__cdecl* removeBus)(Handle,std::uint32_t)=nullptr;
    Bool(__cdecl* createDevice)(Handle,Handle*,std::uint32_t,Bool,std::uint16_t,std::uint16_t,void*)=nullptr;
    Bool(__cdecl* removeDevice)(Handle)=nullptr;
    Bool(__cdecl* setInput)(Handle,const std::uint8_t*,std::uint32_t)=nullptr;
    Bool(__cdecl* setCapture)(Handle,Callback,Handle)=nullptr;
    Bool(__cdecl* attach)(Handle)=nullptr;
    static void __cdecl receive(Handle context,const std::uint8_t* data,std::uint32_t size) noexcept {
        auto& self=*reinterpret_cast<Impl*>(context);
        const bool closing=self.closing.load();
        if(!data) { self.recorder.fail("null capture callback"); return; }
        // This callback never does disk I/O or calls back into VIIPER.
        if(size>=headerSize+8 && size<=maxRecordSize && data[6]==3 && data[7]==0 && data[headerSize]==1)
            self.hostConnected=true;
        if(size>=headerSize+8 && size<=maxRecordSize && data[6]==3 && data[7]==0 &&
           data[headerSize]==2 && !closing) {
            self.lost=true; self.recorder.fail("virtual host disconnected unexpectedly");
        }
        self.recorder.submit({data,size},closing);
    }
};
WindowsCaptureBackend::WindowsCaptureBackend(RawFeedbackSink& r):impl_(std::make_unique<Impl>(r)) {}
WindowsCaptureBackend::~WindowsCaptureBackend() { std::string ignored; close(ignored); }
bool WindowsCaptureBackend::open(const std::filesystem::path& path,std::string& error) {
    auto& s=*impl_;
    try {
        if(s.module) throw std::runtime_error("capture backend is single-use");
        s.module=LoadLibraryExW(std::filesystem::absolute(path).c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!s.module) throw std::runtime_error("could not load libVIIPER (Win32 "+std::to_string(GetLastError())+")");
        symbol(s.module,"GetASBCaptureCapabilities",s.capabilities);
        if((s.capabilities(1)&7)!=7) throw std::runtime_error("raw audio/HID/events ABI 1 required; summary fallback is forbidden");
        symbol(s.module,"NewUSBServerASBLoopback",s.newServer);
        symbol(s.module,"CloseUSBServer",s.closeServer);
        symbol(s.module,"CreateUSBBus",s.createBus);
        symbol(s.module,"RemoveUSBBus",s.removeBus);
        symbol(s.module,"CreateDualSenseDevice",s.createDevice);
        symbol(s.module,"RemoveDualSenseDevice",s.removeDevice);
        symbol(s.module,"SetDualSenseASBInputState",s.setInput);
        symbol(s.module,"SetDualSenseASBCaptureCallback",s.setCapture);
        symbol(s.module,"AttachDualSenseASBDevice",s.attach);
        std::array<char,3> address{':','0','\0'};
        ServerConfig config{address.data(),10000,5000,1};
        if(!s.newServer(&config,&s.server,nullptr)||!s.server) throw std::runtime_error("virtual USB server creation failed");
        if(!s.createBus(s.server,&s.bus)||!s.bus) throw std::runtime_error("virtual USB bus creation failed");
        GUID identity{};
        if(FAILED(CoCreateGuid(&identity))) throw std::runtime_error("cannot generate capture device identity");
        const auto* identityBytes=reinterpret_cast<const unsigned char*>(&identity);
        std::ostringstream serial,mac; serial << "ASBC" << std::hex << std::uppercase << std::setfill('0');
        mac << "02" << std::hex << std::uppercase << std::setfill('0');
        for(unsigned i=0;i<6;++i) serial << std::setw(2) << static_cast<unsigned>(identityBytes[i]);
        for(unsigned i=6;i<11;++i) mac << ':' << std::setw(2) << static_cast<unsigned>(identityBytes[i]);
        s.serial=serial.str(); s.mac=mac.str();
        DeviceMeta meta{s.serial.c_str(),s.mac.c_str(),nullptr,0,0,0,nullptr};
        // autoAttach=false is critical: do not miss host setup/first packets.
        if(!s.createDevice(s.server,&s.device,s.bus,0,0,0,&meta)||!s.device)
            throw std::runtime_error("virtual DualSense creation failed");
        if(!s.setCapture(s.device,&Impl::receive,reinterpret_cast<Handle>(&s)))
            throw std::runtime_error("raw capture registration failed");
        const auto neutral=dualsense::buildNeutralViiperInput();
        if(!s.setInput(s.device,neutral.data(),static_cast<std::uint32_t>(neutral.size())))
            throw std::runtime_error("neutral input initialization failed");
        if(!s.attach(s.device)) throw std::runtime_error("virtual attachment failed; verify the USBip UDE driver");
        const auto connectedDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!s.hostConnected && !s.recorder.failed() && std::chrono::steady_clock::now()<connectedDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(!s.hostConnected || s.recorder.failed()) throw std::runtime_error("virtual host did not establish a capture stream");
        return true;
    } catch(const std::exception& e) {
        error=e.what(); s.recorder.fail("virtual capture startup failed");
        std::string ignored; close(ignored); return false;
    }
}
bool WindowsCaptureBackend::update(const dualsense::DualSenseInputState& state,std::string& error) {
    auto& s=*impl_; const auto bytes=dualsense::buildViiperInput(state);
    if(!s.device || !s.setInput(s.device,bytes.data(),static_cast<std::uint32_t>(bytes.size()))) {
        error="virtual input update failed"; s.recorder.fail("virtual input update failed"); return false;
    }
    return true;
}
bool WindowsCaptureBackend::close(std::string& error) noexcept {
    auto& s=*impl_; s.closing=true;
    bool ok=true;
    // Unregister is a quiescence barrier implemented by the Go callback mutex.
    // Keep c-shared Go runtime resident, as the legacy adapter does.
    if(s.device) {
        if(!s.setCapture(s.device,nullptr,0)) ok=false;
        if(!s.removeDevice(s.device)) ok=false;
        s.device=0;
    }
    if(s.bus && s.server) { if(!s.removeBus(s.server,s.bus)) ok=false; s.bus=0; }
    if(s.server) { if(!s.closeServer(s.server)) ok=false; s.server=0; }
    if(!ok) {
        s.recorder.fail("virtual detach/cleanup failed");
        try { error="virtual cleanup failed; inspect for orphan device"; } catch(...) {}
    }
    return ok;
}
bool WindowsCaptureBackend::disconnected() const noexcept { return impl_->lost.load(); }
std::string WindowsCaptureBackend::serial() const { return impl_->serial; }
} // namespace asb::capture
