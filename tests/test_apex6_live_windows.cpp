#include "apex6/experiment/WindowsIoTestHooks.h"
#include "apex6/experiment/FakeIo.h"
#include "capture/WindowsCaptureBackend.h"
#include "apex6_rehearsal_fixture.h"
#include <iostream>
#include <map>
#include <thread>
#include <algorithm>
using namespace asb::apex6;
using namespace asb::apex6::experiment;
namespace live=asb::apex6::live;
void check(bool b,const char* why){if(!b)throw ProtocolError(why);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,"expected rejection");}
struct Kernel {
    FakeIo peer{gripRehearsalFixture()};unsigned writes=0;bool cancelled=false;Time completionDelay{},clockStep{};
    struct Operation{bool read;void* buffer;DWORD n;HANDLE event;};std::map<OVERLAPPED*,Operation> ops;
    WindowsIoHooks hooks(){return {
        [&](bool read,HANDLE,void* p,DWORD n,OVERLAPPED* o,DWORD& e)->BOOL{ops[o]={read,p,n,o->hEvent};if(!read){++writes;peer.write({static_cast<std::uint8_t*>(p),n},peer.now()+Time(600000));}e=ERROR_IO_PENDING;return FALSE;},
        [&](HANDLE event,DWORD)->DWORD{for(auto& [key,op]:ops)if(op.event==event)return op.read&&peer.incoming.empty()?WAIT_TIMEOUT:WAIT_OBJECT_0;return WAIT_FAILED;},
        [&](HANDLE,OVERLAPPED* o,DWORD& n,DWORD& e)->BOOL{auto op=ops.at(o);ops.erase(o);if(op.read){if(peer.incoming.empty()){e=ERROR_OPERATION_ABORTED;return FALSE;}auto r=peer.read(peer.now()+Time(600000),false);std::copy(r.bytes.begin(),r.bytes.end(),static_cast<std::uint8_t*>(op.buffer));n=static_cast<DWORD>(r.bytes.size());}else{n=op.n;peer.clock+=completionDelay;}e=0;return TRUE;},
        [](HANDLE,OVERLAPPED*){},[&]{peer.clock+=clockStep;return peer.now();}};}
};
GripBaseline physicalBaseline(){auto b=gripRehearsalFixture();b.physicalOrigin=true;b.binding.access=AccessMode::Shared;return b;}
live::Approval consent(){live::Approval a;auto& p=a.checkpoints;p.manifestHash=std::string(64,'a');p.confirmedUnixSeconds=1000;p.approved=p.observer=p.powerOffReady=p.normalVibration=p.knownWritersQuiesced=p.directUsb=p.failStopAccepted=p.backgroundRiskAccepted=p.restorationRiskAccepted=p.reducedAuditAccepted=p.powerCycled=p.restoreOnlyAccepted=a.liveAccepted=true;return a;}
struct Harness {
    Kernel kernel;GripBaseline b=physicalBaseline();live::Authorization authorization;
    std::unique_ptr<WindowsTransport> io;
    explicit Harness(unsigned seconds=1):authorization(live::Authorization::approve(b,{seconds,1},consent(),std::string(64,'a'),1000)),io(makeLiveIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;},[&]{return kernel.cancelled;})){}
    IoResult send(const Frame& f){return io->write(b.binding.layout.wrap(f),kernel.peer.now()+Time(600000));}
    void request(const Request& r){check(send(frame(r.command,r.payload)).status==Completion::Complete,"native request failed");check(io->read(kernel.peer.now()+Time(600000),false).status==Completion::Complete,"native reply failed");}
    void enter(){for(auto r:gripBaselinePlan())request(r);request(gripLifecyclePlan(b)[0]);check(send(gripWaveform({})).status==Completion::Complete,"neutral lead failed");}
    void refused(Frame f){auto n=kernel.writes;check(send(f).status==Completion::Failed&&kernel.writes==n,"native rejected command reached kernel");check(send(gripWaveform({})).status==Completion::Failed&&kernel.writes==n,"native failure not latched");}
};
void nativeTests(){
    for(unsigned kind=0;kind<7;++kind){Harness h;h.enter();h.kernel.peer.clock=Time(8000);auto f=gripWaveform({});if(kind==0)f=frame(0xa1,{});if(kind==1)f=frame(0x53,Bytes{1,0x12,0});if(kind==2)f[4]|=4;if(kind==3)f[5]=129;if(kind==4)f[6]=224;if(kind==5)f[7]=31;if(kind==6)f[31]^=1;if(kind>=2&&kind<=5)f=frame(0x57,std::span(f).subspan(4,25));h.refused(f);}
    {Harness h;h.refused(gripWaveform({}));}
    {Harness h;h.request(gripBaselinePlan()[0]);h.refused(frame(1));}
    {Harness h;h.enter();h.kernel.peer.clock=Time(1000000);h.refused(gripWaveform({}));}
    {Harness h;h.enter();h.kernel.peer.clock=Time(19017);if(live::enforceDispatchLateness)h.refused(gripWaveform({}));else{check(h.send(gripWaveform({})).status==Completion::Complete,"debug native late dispatch rejected");h.kernel.peer.clock+=Time(7999);h.refused(gripWaveform({}));}}
    {Harness h;h.enter();h.kernel.peer.clock=Time(7999);h.refused(gripWaveform({}));}
    {Harness h;h.enter();h.kernel.peer.clock=Time(8000);h.kernel.completionDelay=Time(4000);auto before=h.kernel.writes;check(h.send(gripWaveform({})).status==Completion::Failed&&h.kernel.writes==before+1,"native late completion accepted");h.refused(gripWaveform({}));}
    {Harness h;h.enter();h.kernel.cancelled=true;h.refused(gripWaveform({}));}
    {Harness h;h.enter();h.kernel.peer.clock=Time(8000);h.io->stopLive(h.kernel.peer.now());check(h.send(gripWaveform({})).status==Completion::Complete,"tail 1");h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"tail 2");h.kernel.peer.clock+=Time(8000);h.refused(gripWaveform({}));}
    {Harness h;h.enter();h.kernel.peer.clock=Time(8000);h.io->stopLive(h.kernel.peer.now());std::array<StereoSample,8> samples{};samples[0]={.1,0};h.refused(gripWaveform(samples));}
    {Harness h;h.enter();h.kernel.peer.clock=Time(8000);h.io->stopLive(h.kernel.peer.now());check(h.send(gripWaveform({})).status==Completion::Complete,"tail");h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"tail");h.kernel.peer.clock+=Time(8000);auto modes=gripLifecyclePlan(h.b);for(unsigned i=1;i<4;++i)h.request(modes[i]);for(auto r:gripBaselinePlan())h.request(r);check(h.kernel.writes==35,"completed native budgets");h.refused(frame(1));}
    // Real clocks advance between the caller's Q timestamp and native admission.
    // Exercise the entire shutdown; a frozen fake clock hid this regression.
    for(unsigned seconds:{1u,0u}){
        Harness h(seconds);h.enter();h.kernel.peer.clock=Time(8000);h.kernel.clockStep=Time(1);
        const auto requested=h.io->now();h.io->stopLive(requested);
        check(h.send(gripWaveform({})).status==Completion::Complete,"advancing-clock tail 1");
        h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"advancing-clock tail 2");
        h.kernel.peer.clock+=Time(8000);auto modes=gripLifecyclePlan(h.b);
        for(unsigned i=1;i<4;++i)h.request(modes[i]);
        for(auto r:gripBaselinePlan())h.request(r);
        check(h.kernel.writes==35,"advancing-clock Q missed restores/postflight");h.refused(frame(1));
    }
    {Harness h;h.enter();h.kernel.peer.clock=Time(8000);h.kernel.cancelled=true;rejects([&]{h.io->stopLive(h.io->now());});h.refused(gripWaveform({}));}
    {Harness h;h.enter();h.kernel.peer.clock=Time(8000);rejects([&]{h.io->stopLive(Time(9000));});h.refused(gripWaveform({}));}
    {Harness h;rejects([&]{h.authorization.consume();});}
    auto a=consent();rejects([&]{live::Authorization::approve(physicalBaseline(),{},a,std::string(64,'b'),1000);});rejects([&]{live::Authorization::approve(physicalBaseline(),{},a,std::string(64,'a'),1301);});
    auto b=physicalBaseline();b.mapping[24]^=1;rejects([&]{live::Authorization::approve(b,{},a,std::string(64,'a'),1000);});
    {auto active=live::Authorization::approve(physicalBaseline(),{0,1},consent(),std::string(64,'a'),1000);rejects([&]{active.activate(1301);});active.activate(1001);active.check(10000);rejects([&]{active.activate(1002);});rejects([&]{active.check(1000);});active.consume();rejects([&]{active.consume();});}
    {Harness h(0);h.enter();std::size_t exported=0;for(unsigned i=0;i<40000;++i){h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"continuous native stream hit old time/packet limit");h.io->drainLiveTimings([&](const NativeTiming&){++exported;});}check(exported>131072&&h.io->timings().empty()&&h.io->timingComplete(),"native timing drain lost capacity/evidence");h.kernel.cancelled=true;h.refused(gripWaveform({}));}
    {Harness h(0);h.enter();rejects([&]{h.io->drainLiveTimings([](const NativeTiming&){throw ProtocolError("evidence failure");});});h.refused(gripWaveform({}));}
}
void backendTests(const char* path){
    auto library=std::filesystem::absolute(path);auto module=LoadLibraryW(library.c_str());check(module!=nullptr,"fake DLL load");auto configure=reinterpret_cast<void(*)(unsigned)>(GetProcAddress(module,"ConfigureCaptureFake"));check(configure!=nullptr,"fake configure");
    for(unsigned iteration=0;iteration<20;++iteration){configure(0);live::RawQueue queue;asb::capture::WindowsCaptureBackend backend(queue);std::string error;check(backend.open(library,error),"queue backend attach");std::this_thread::sleep_for(std::chrono::milliseconds(2));check(backend.update({},error),"queue backend input");check(backend.close(error),"callback quiescence");auto records=queue.metrics().records;std::this_thread::sleep_for(std::chrono::milliseconds(2));check(records==queue.metrics().records&&!queue.failed(),"callback outlived unregister");unsigned count=0;while(queue.pop())++count;check(count==records,"queue lost owned records");}
}
int main(int argc,char** argv){try{check(argc==2,"fake DLL required");nativeTests();backendTests(argv[1]);std::cout<<"Native live guard and callback quiescence passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
