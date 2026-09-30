#include "apex6/experiment/WindowsIoTestHooks.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6/dongle/Pulse.h"
#include "apex6_rehearsal_fixture.h"
#include <algorithm>
#include <iostream>
#include <map>
using namespace asb::apex6;
using namespace asb::apex6::experiment;
void check(bool ok,const char* why){if(!ok)throw ProtocolError(why);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const ProtocolError&){rejected=true;}check(rejected,"expected rejection");}
struct Kernel {
    FakeIo peer{gripRehearsalFixture()};unsigned writes=0;Time delay{};
    struct Operation{bool read;void* buffer;DWORD count;HANDLE event;};std::map<OVERLAPPED*,Operation> pending;
    WindowsIoHooks hooks(){return {
        [&](bool read,HANDLE,void* buffer,DWORD count,OVERLAPPED* op,DWORD& error)->BOOL{
            pending[op]={read,buffer,count,op->hEvent};if(!read){++writes;peer.write({static_cast<std::uint8_t*>(buffer),count},peer.now()+Time(600000));}error=ERROR_IO_PENDING;return FALSE;},
        [&](HANDLE event,DWORD)->DWORD{for(const auto& [key,op]:pending)if(op.event==event)return op.read&&peer.incoming.empty()?WAIT_TIMEOUT:WAIT_OBJECT_0;return WAIT_FAILED;},
        [&](HANDLE,OVERLAPPED* key,DWORD& count,DWORD& error)->BOOL{const auto op=pending.at(key);pending.erase(key);
            if(op.read){if(peer.incoming.empty()){error=ERROR_OPERATION_ABORTED;return FALSE;}const auto r=peer.read(peer.now()+Time(600000),false);std::copy(r.bytes.begin(),r.bytes.end(),static_cast<std::uint8_t*>(op.buffer));count=static_cast<DWORD>(r.bytes.size());}
            else{count=op.count;peer.clock+=delay;}error=0;return TRUE;},
        [](HANDLE,OVERLAPPED*){},[&]{return peer.now();}};}
};
struct Harness {
    Kernel kernel;GripBaseline baseline=gripRehearsalFixture();std::unique_ptr<WindowsTransport> io;
    Harness(){baseline.physicalOrigin=true;baseline.binding.access=AccessMode::Shared;io=makeDongleBaselineIoForTest(baseline.binding,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());}
    IoResult send(const Frame& f){return io->write(baseline.binding.layout.wrap(f),kernel.peer.now()+Time(600000));}
    void request(const Request& r){check(send(frame(r.command,r.payload)).status==Completion::Complete,"request rejected");check(io->read(kernel.peer.now()+Time(600000),false).status==Completion::Complete,"reply rejected");}
    void baselines(){for(unsigned i=0;i<2;++i){for(const auto& r:gripBaselinePlan())request(r);request({4,{},16});}}
    void promote(live::ReplyBoundary boundary=live::ReplyBoundary::None){promoteDongleNeutralTransport(*io,baseline,[]{return false;},[]{},boundary);}
    void enter(live::ReplyBoundary boundary=live::ReplyBoundary::None){baselines();promote(boundary);for(const auto& r:gripBaselinePlan())request(r);request(gripLifecyclePlan(baseline)[0]);check(send(gripWaveform({})).status==Completion::Complete,"lead rejected");}
    void enterPulse(dongle::PulseSide side=dongle::PulseSide::Left){baselines();promoteDonglePulseTransport(*io,baseline,[]{return false;},[]{},side);for(const auto& r:gripBaselinePlan())request(r);request(gripLifecyclePlan(baseline)[0]);check(send(gripWaveform({})).status==Completion::Complete,"pulse lead rejected");}
    void tail(live::ReplyBoundary boundary){enter(boundary);for(unsigned i=0;i<8;++i){kernel.peer.clock+=Time(8000);check(send(gripWaveform({})).status==Completion::Complete,"stream rejected");}
        io->stopLive(kernel.peer.now());for(unsigned i=0;i<2;++i){kernel.peer.clock+=Time(8000);check(send(gripWaveform({})).status==Completion::Complete,"tail rejected");}kernel.peer.clock+=Time(8000);}
    void refused(const Frame& f){const auto before=kernel.writes;check(send(f).status==Completion::Failed&&kernel.writes==before,"rejected frame reached kernel");check(send(gripWaveform({})).status==Completion::Failed&&kernel.writes==before,"failure not latched");}
};
int main(){try{
    for(unsigned count:{0u,14u,15u,28u,29u}){Harness h;auto plan=gripBaselinePlan();plan.push_back({4,{},16});for(unsigned i=0;i<count;++i)h.request(plan[i%15]);rejects([&]{h.promote();});h.refused(frame(1));}
    {Harness h;h.baselines();h.refused(frame(1));}
    for(unsigned channel=0;channel<3;++channel){Harness h;h.enter();h.kernel.peer.clock+=Time(8000);std::array<std::vector<double>,3> samples;samples[channel]={.0625};h.refused(waveform(samples,{channel==0,true,true}));}
    {Harness h;h.enter();for(unsigned i=0;i<10;++i){h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"neutral within budget rejected");}h.kernel.peer.clock+=Time(8000);h.refused(gripWaveform({}));}
    {Harness h;h.enter();h.kernel.peer.clock+=Time(8000);h.kernel.delay=Time(4000);check(h.send(gripWaveform({})).status==Completion::Failed,"4-ms native deadline relaxed");h.refused(gripWaveform({}));}
    {Harness h;h.enter();h.kernel.peer.clock+=Time(7999);h.refused(gripWaveform({}));}
    {Harness h;h.enter();for(unsigned i=0;i<8;++i){h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"test packet rejected");}
        h.io->stopLive(h.kernel.peer.now());for(unsigned i=0;i<2;++i){h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"tail rejected");}
        h.kernel.peer.clock+=Time(8000);const auto modes=gripLifecyclePlan(h.baseline);for(unsigned i=1;i<4;++i)h.request(modes[i]);for(const auto& r:gripBaselinePlan())h.request(r);
        check(h.kernel.writes==73,"single-handle total write count");h.refused(frame(1));}
    const auto boundary=live::ReplyBoundary::DongleUidDiagnostic;
    {Harness h;h.tail(boundary);const auto modes=gripLifecyclePlan(h.baseline);for(unsigned i=1;i<4;++i){h.request({4,{},16});h.request(modes[i]);}for(const auto& r:gripBaselinePlan())h.request(r);check(h.kernel.writes==76,"native UID boundary total");h.refused(frame(4));}
    {Harness h;h.tail(boundary);const auto mode=gripLifecyclePlan(h.baseline)[1];h.refused(frame(mode.command,mode.payload));}
    {Harness h;h.tail(boundary);h.request({4,{},16});h.refused(frame(4));}
    {Harness h;h.tail(live::ReplyBoundary::None);h.refused(frame(4));}
    {Harness h;h.enter(boundary);h.refused(frame(4));}
    {Harness h;h.tail(boundary);h.kernel.peer.snapshot.unit[0]^=1;check(h.send(frame(4)).status==Completion::Complete,"boundary query write");check(h.io->read(h.kernel.peer.now()+Time(600000),false).status==Completion::Failed,"native wrong UID accepted");h.refused(frame(4));}
    {Harness h;h.enterPulse();for(unsigned i=0;i<32;++i){h.kernel.peer.clock+=Time(8000);check(h.send(dongle::pulsePacket(i)).status==Completion::Complete,"native pulse packet rejected");}
        h.io->stopLive(h.kernel.peer.now());for(unsigned i=0;i<2;++i){h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"pulse tail rejected");}
        h.kernel.peer.clock+=Time(8000);const auto modes=gripLifecyclePlan(h.baseline);for(unsigned i=1;i<4;++i){h.request({4,{},16});h.request(modes[i]);}for(const auto& r:gripBaselinePlan())h.request(r);check(h.kernel.writes==100,"native pulse total writes");h.refused(frame(4));}
    {Harness h;h.enterPulse();h.kernel.peer.clock+=Time(8000);h.refused(dongle::pulsePacket(1));}
    {Harness h;h.enterPulse();h.kernel.peer.clock+=Time(7999);h.refused(dongle::pulsePacket(0));}
    {Harness h;h.enterPulse();h.kernel.peer.clock+=Time(8000);h.kernel.delay=Time(4000);check(h.send(dongle::pulsePacket(0)).status==Completion::Failed,"pulse native deadline relaxed");h.refused(gripWaveform({}));}
    for(unsigned channel=0;channel<3;++channel){Harness h;h.enterPulse();h.kernel.peer.clock+=Time(8000);std::array<std::vector<double>,3> samples;samples[channel]={.125};h.refused(waveform(samples,{true,true,true}));}
    {Harness h;h.enterPulse();for(unsigned i=0;i<34;++i){h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"pulse within budget");}h.kernel.peer.clock+=Time(8000);h.refused(gripWaveform({}));}
    for(auto side:{dongle::PulseSide::Right,dongle::PulseSide::Both}){
        {Harness h;h.enterPulse(side);for(unsigned i=0;i<32;++i){h.kernel.peer.clock+=Time(8000);check(h.send(dongle::pulsePacket(i,side)).status==Completion::Complete,"native selected pulse rejected");}
            h.io->stopLive(h.kernel.peer.now());for(unsigned i=0;i<2;++i){h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"selected pulse tail");}h.kernel.peer.clock+=Time(8000);const auto modes=gripLifecyclePlan(h.baseline);for(unsigned i=1;i<4;++i){h.request({4,{},16});h.request(modes[i]);}for(const auto& q:gripBaselinePlan())h.request(q);check(h.kernel.writes==100,"native selected pulse budget");}
        {Harness h;h.enterPulse(side);h.kernel.peer.clock+=Time(8000);h.refused(dongle::pulsePacket(0,dongle::PulseSide::Left));}
        {Harness h;h.enterPulse(side);h.kernel.peer.clock+=Time(7999);h.refused(dongle::pulsePacket(0,side));}
        {Harness h;h.enterPulse(side);h.kernel.peer.clock+=Time(8000);h.kernel.delay=Time(4000);check(h.send(dongle::pulsePacket(0,side)).status==Completion::Failed,"selected pulse deadline relaxed");h.refused(gripWaveform({}));}
    }
    for(auto side:{dongle::PulseSide::None,static_cast<dongle::PulseSide>(999)}){Harness h;h.baselines();rejects([&]{promoteDonglePulseTransport(*h.io,h.baseline,[]{return false;},[]{},side);});h.refused(frame(4));}
    {Harness h;rejects([&]{promoteDongleLiveTransport(*h.io,h.baseline,{0,1,false},[]{return false;},[]{});});h.refused(frame(4));}
    {Harness h;h.baselines();promoteDongleLiveTransport(*h.io,h.baseline,{0,1,false},[]{return false;},[]{});
        for(const auto& r:gripBaselinePlan())h.request(r);h.request(gripLifecyclePlan(h.baseline)[0]);check(h.send(gripWaveform({})).status==Completion::Complete,"live lead");
        for(unsigned i=0;i<400;++i){h.kernel.peer.clock+=Time(8000);check(h.send(dongle::pulsePacket(i%32,dongle::PulseSide::Both)).status==Completion::Complete,"live output beyond pulse budget");h.io->drainLiveTimings([](const NativeTiming&){});}
        h.io->stopLive(h.kernel.peer.now());for(unsigned i=0;i<2;++i){h.kernel.peer.clock+=Time(8000);check(h.send(gripWaveform({})).status==Completion::Complete,"live tail");}h.kernel.peer.clock+=Time(8000);const auto modes=gripLifecyclePlan(h.baseline);for(unsigned i=1;i<4;++i){h.request({4,{},16});h.request(modes[i]);}for(const auto& r:gripBaselinePlan())h.request(r);h.refused(frame(4));}
    {Harness h;h.baselines();promoteDongleLiveTransport(*h.io,h.baseline,{0,1,false},[]{return false;},[]{});h.kernel.peer.clock=Time(1200000000);h.refused(frame(1));}
    std::cout<<"Dongle single-handle promotion, native pulse selection, spacing and deadlines passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
