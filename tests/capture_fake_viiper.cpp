#include <array>
#include <cstdint>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
namespace {
using Callback=void(__cdecl*)(std::uintptr_t,const std::uint8_t*,std::uint32_t);
std::mutex mutex;
Callback callback=nullptr; std::uintptr_t context=0;
unsigned mode=0,created=0; bool initialized=false;
std::uint64_t sequence=0;
std::atomic_bool running{false}; std::thread worker;
void put(std::array<std::uint8_t,104>& b,std::size_t o,std::uint64_t value,std::size_t n=4) {
    for(std::size_t i=0;i<n;++i) b[o+i]=static_cast<std::uint8_t>(value>>(i*8));
}
void emit() {
    std::lock_guard lock(mutex); if(!callback) return;
    std::array<std::uint8_t,104> b{};
    b[0]='A';b[1]='S';b[2]='B';b[3]='R';
    put(b,4,1,2);put(b,6,1,2);put(b,8,b.size());put(b,12,80);
    put(b,16,++sequence,8);put(b,24,1,8);put(b,32,sequence*1000,8);
    put(b,40,1);put(b,48,48000);put(b,52,4,2);put(b,54,16,2);
    put(b,56,1);put(b,60,8);put(b,68,1);put(b,72,1);put(b,84,8);
    put(b,96,0x8000,2);put(b,98,0x7fff,2);put(b,100,0xffff,2);put(b,102,1,2);
    callback(context,b.data(),static_cast<std::uint32_t>(b.size()));
}
}
#define EXPORT extern "C" __declspec(dllexport)
EXPORT std::uint8_t SetDualSenseASBRawOutputCallback(std::uintptr_t,Callback,std::uintptr_t) { return 1; }
EXPORT void ConfigureCaptureFake(unsigned value) { mode=value; created=0; initialized=false; sequence=0; }
EXPORT unsigned CaptureFakeCreated() { return created; }
EXPORT std::uint32_t GetASBCaptureCapabilities(std::uint32_t version) { return version==1 && mode!=1 ? 7:0; }
EXPORT std::uint8_t NewUSBServerASBLoopback(void*,std::uintptr_t* out,void*) { ++created; *out=1; return 1; }
EXPORT std::uint8_t CloseUSBServer(std::uintptr_t) { return 1; }
EXPORT std::uint8_t CreateUSBBus(std::uintptr_t,std::uint32_t* out) { if(mode==3) return 0; *out=1; return 1; }
EXPORT std::uint8_t RemoveUSBBus(std::uintptr_t,std::uint32_t) { return 1; }
EXPORT std::uint8_t CreateDualSenseDevice(std::uintptr_t,std::uintptr_t* out,std::uint32_t,std::uint8_t attach,std::uint16_t,std::uint16_t,void*) {
    if(attach || mode==4) return 0; *out=1; return 1;
}
EXPORT std::uint8_t SetDualSenseASBCaptureCallback(std::uintptr_t,Callback cb,std::uintptr_t ctx) {
    if(mode==5 && cb) return 0;
    std::lock_guard lock(mutex); callback=cb;context=ctx;return 1;
}
EXPORT std::uint8_t SetDualSenseASBInputState(std::uintptr_t,const std::uint8_t*,std::uint32_t size) { initialized=size==33 && mode!=6;return initialized; }
EXPORT std::uint8_t AttachDualSenseASBDevice(std::uintptr_t) {
    if(!callback || !initialized || mode==2) return 0;
    {
        std::lock_guard lock(mutex);
        std::array<std::uint8_t,104> event{};
        event[0]='A';event[1]='S';event[2]='B';event[3]='R';
        put(event,4,1,2);put(event,6,3,2);put(event,8,88);put(event,12,80);
        put(event,16,++sequence,8);put(event,24,1,8);put(event,32,sequence*1000,8);
        put(event,60,8);put(event,68,4);put(event,72,1);put(event,80,1);
        callback(context,event.data(),88);
    }
    emit(); running=true;
    worker=std::thread([]{while(running) {emit();std::this_thread::sleep_for(std::chrono::milliseconds(1));}});
    return 1;
}
EXPORT std::uint8_t RemoveDualSenseDevice(std::uintptr_t) {
    running=false; if(worker.joinable()) worker.join(); return callback==nullptr;
}
