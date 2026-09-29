#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "capture/FeedbackRecorder.h"
#include "dualsense/DualSenseInput.h"
#include <iostream>
#include <stdexcept>

namespace {
using Handle=std::uintptr_t;using Bool=std::uint8_t;
using Callback=void(__cdecl*)(Handle,const std::uint8_t*,std::uint32_t);
struct Config { char* address;std::uint64_t connectionMs,handlerMs;std::uint32_t flushMs; };
template<class F> F symbol(HMODULE library,const char* name) {
    auto function=reinterpret_cast<F>(GetProcAddress(library,name));
    if(!function) throw std::runtime_error(std::string("missing export: ")+name);
    return function;
}
void check(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
void __cdecl receive(Handle context,const std::uint8_t* bytes,std::uint32_t size) noexcept {
    reinterpret_cast<asb::capture::FeedbackRecorder*>(context)->submit({bytes,size});
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2 || !std::filesystem::exists(argv[1])) { std::cout << "SKIP: build raw libVIIPER first\n";return 77; }
        auto library=LoadLibraryExW(std::filesystem::absolute(argv[1]).c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        check(library!=nullptr,"load actual Go c-shared library");
        auto capabilities=reinterpret_cast<std::uint32_t(*)(std::uint32_t)>(GetProcAddress(library,"GetASBCaptureCapabilities"));
        if(!capabilities) { std::cout << "SKIP: legacy DLL; build raw libVIIPER first\n";return 77; }
        check(capabilities(1)==7 && capabilities(999)==0,"capability negotiation");
        auto newServer=symbol<Bool(*)(Config*,Handle*,void*)>(library,"NewUSBServerASBLoopback");
        auto closeServer=symbol<Bool(*)(Handle)>(library,"CloseUSBServer");
        auto newBus=symbol<Bool(*)(Handle,std::uint32_t*)>(library,"CreateUSBBus");
        auto removeBus=symbol<Bool(*)(Handle,std::uint32_t)>(library,"RemoveUSBBus");
        auto newDevice=symbol<Bool(*)(Handle,Handle*,std::uint32_t,Bool,std::uint16_t,std::uint16_t,void*)>(library,"CreateDualSenseDevice");
        auto removeDevice=symbol<Bool(*)(Handle)>(library,"RemoveDualSenseDevice");
        auto setCapture=symbol<Bool(*)(Handle,Callback,Handle)>(library,"SetDualSenseASBCaptureCallback");
        auto setInput=symbol<Bool(*)(Handle,const std::uint8_t*,std::uint32_t)>(library,"SetDualSenseASBInputState");
        const auto directory=std::filesystem::temp_directory_path()/("asb-real-abi-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        asb::capture::FeedbackRecorder recorder(directory);std::string error;check(recorder.start(error),"recorder start");
        Handle server=0,device=0;std::uint32_t bus=0;
        struct Cleanup {
            Handle& server;Handle& device;std::uint32_t& bus;
            decltype(setCapture) clear;decltype(removeDevice) deviceRemove;decltype(removeBus) busRemove;decltype(closeServer) serverClose;
            void close() {
                if(device) {clear(device,nullptr,0);deviceRemove(device);device=0;}
                if(bus && server) {busRemove(server,bus);bus=0;}
                if(server) {serverClose(server);server=0;}
            }
            ~Cleanup(){close();}
        } cleanup{server,device,bus,setCapture,removeDevice,removeBus,closeServer};
        char address[] = ":0";Config config{address,1000,1000,1};
        check(newServer(&config,&server,nullptr)&&server,"loopback server");
        check(newBus(server,&bus)&&bus,"in-memory virtual bus");
        // autoAttach=false. This test never calls AttachDualSenseASBDevice and
        // never opens the virtual-host driver, HID output, or a physical controller.
        check(newDevice(server,&device,bus,0,0,0,nullptr)&&device,"unattached virtual device");
        check(setCapture(device,receive,reinterpret_cast<Handle>(&recorder)),"actual Go -> C++ callback registration");
        const auto neutral=asb::dualsense::buildNeutralViiperInput();
        check(setInput(device,neutral.data(),static_cast<std::uint32_t>(neutral.size())),"actual C++ -> Go input ABI");
        check(setCapture(device,nullptr,0),"actual callback quiescence");
        cleanup.close();
        check(recorder.finish("software-only-unattached-real-dll",asb::capture::sha256File(argv[1]),error),"actual callback envelope decoded/finalized");
        std::cout << "Real DLL ABI passed without host attachment: " << directory.string() << '\n';return 0;
    } catch(const std::exception& e) {std::cerr << e.what() << '\n';return 1;}
}
