// Real-clock scheduler probe. Links the synthetic peer and pure policy only;
// no HID discovery, WindowsIo, or physical transport is linked or opened.
#include "apex6/experiment/WindowsPulseWait.h"
#include "apex6/experiment/PulseEvidence.h"
#include "apex6/experiment/Evidence.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6_rehearsal_fixture.h"
#include <fstream>
#include <iostream>

using namespace asb::apex6;
using namespace asb::apex6::experiment;
Time realNow(){return std::chrono::duration_cast<Time>(std::chrono::steady_clock::now().time_since_epoch());}
struct Event {HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Event(){if(value)CloseHandle(value);}};
class TimedPeer:public Io {
public:
    TimedPeer(const GripBaseline& b,HANDLE cancel):fake(b),native(b,realNow()),wait(cancel,realNow){}
    Time now()const override{return realNow();}
    bool physical()const override{return false;}
    const Binding& binding()const override{return fake.binding();}
    bool stillSameDevice()override {
        // Declared workload model, not measured device-presence latency.
        const auto end=now()+Time(25);while(now()<end)std::atomic_signal_fence(std::memory_order_seq_cst);return true;
    }
    IoResult read(Time deadline,bool poll)override {
        fake.clock=now();auto r=fake.read(deadline,poll);native.afterRead(r,now());return r;
    }
    IoResult write(std::span<const std::uint8_t> wire,Time deadline)override {
        const auto s=now();native.beforeWrite(wire,s);fake.clock=s;auto r=fake.write(wire,deadline);
        wait.waitUntil(s+Time(1000));const auto c=now();r.submittedAt=s;r.completedAt=c;
        native.afterWrite(r,s,c);return r;
    }
    FakeIo fake;PulseNativeGuard native;WindowsPulseWait wait;
};
int wmain(int argc,wchar_t** argv){try {
    if(argc!=2)throw ProtocolError("usage: timing probe NEW_OUTPUT_DIRECTORY (20 offline trials)");
    const std::filesystem::path root=argv[1];if(!std::filesystem::create_directory(root))throw ProtocolError("output must be new");
    Event cancel;if(!cancel.value)throw ProtocolError("cancel event creation failed");
    unsigned passed=0;Time maximum{};std::ofstream report(root/"probe.json");
    report<<"{\"schema\":\"asb.apex6.offline-timing-probe.v1\",\"hid_opens\":0,\"trials\":20,\"modeled_presence_us\":25,\"modeled_write_us\":1000,\"runs\":[";
    for(unsigned i=0;i<20;++i) {
        const auto b=gripRehearsalFixture();TimedPeer io(b,cancel.value);Evidence trace(root/std::to_string(i));Session session(io,trace);
        WindowsPulseWait scheduler(cancel.value,realNow);
        auto r=rehearseGripPulse(session,b,{[&](Time due){scheduler.waitUntil(due);},[&]{return scheduler.cancelled();}});
        if(!trace.finish())r.failure="trace failed";
        if(r.complete())++passed;for(const auto& p:r.packets)maximum=std::max(maximum,p.lateness);
        if(i)report<<',';report<<pulseResultJson(r);
        std::cout<<"trial "<<i<<": "<<(r.complete()?"complete":r.failure)<<"; waveform writes="<<r.waveformWrites<<'\n';
    }
    report<<"],\"passed\":"<<passed<<",\"failed\":"<<20-passed<<",\"maximum_accepted_lateness_us\":"<<maximum.count()<<"}\n";
    report.flush();if(!report)throw ProtocolError("probe evidence write failed");
    std::cout<<passed<<"/20 timing trials complete; maximum accepted lateness "<<maximum.count()<<" us; no HID opens\n";
    return passed==20?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
