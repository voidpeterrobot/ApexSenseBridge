#include "apex6/experiment/PulseEvidence.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6_rehearsal_fixture.h"
#include <iostream>
using namespace asb::apex6;
using namespace asb::apex6::experiment;
namespace {
unsigned checks=0;
void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const ProtocolError&){caught=true;}check(caught,"expected refusal");}
const auto ack=unhex("005aa5530100000000000000000000000000000000000000000000000000000054");
const auto anomaly=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");
struct TraceLog:Trace {
    unsigned count=0,failAt=0;bool good=true;
    void record(Time,const std::string&,std::span<const std::uint8_t>,std::uint64_t,std::uint64_t)override{if(++count==failAt){good=false;throw ProtocolError("trace failure");}}
    bool healthy()const override{return good;}
};
struct Harness {
    GripBaseline b=gripRehearsalFixture();FakeIo io{b};TraceLog trace;Session s{io,trace};
    PulseControl control{[&](Time due){io.clock=due;},[]{return false;}};
    GripPulseResult run(){return rehearseGripPulse(s,b,control);}
    void stopped(){const auto writes=io.writes,reads=io.reads;rejects([&]{s.exchange({1,{},25});});check(io.writes==writes&&io.reads==reads,"traffic after latched failure");}
};
std::vector<Bytes> reports(const GripBaseline& b) {
    std::vector<Bytes> out;for(const auto& plan:{gripBaselinePlan(),gripPulsePlan(b),gripBaselinePlan()})for(const auto& r:plan)out.push_back(b.binding.layout.wrap(frame(r.command,r.payload)));return out;
}
void successAndFaults() {
    DspMetrics metrics;const auto generated=gripPulseFrames(&metrics);
    check(metrics.peakBeforeLimit==.75&&metrics.peakAfterLimit==.75&&metrics.clippedSamples==0&&metrics.overrangeSamples==0,"pulse strength metrics");
    const auto full=unhex("80c3dfc3803c203c"),edge=unhex("80a1afa1805e505e");
    for(unsigned p=0;p<11;++p)for(unsigned i=0;i<8;++i) {
        const auto expected=(p==0||p>=9)?128:(p==1||p==8)?edge[i]:full[i];
        check(generated[p][6+3*i]==expected&&generated[p][5+3*i]==128&&generated[p][7+3*i]==128,"v4 exact 12x strength and channel bytes");
    }
    Harness good;const auto r=good.run();check(r.complete(),r.failure.c_str());check(good.io.writes==43&&r.queryWrites==28&&r.modeWrites==4&&r.waveformWrites==11,"pulse counts");
    check(r.deviceStateUncertain&&!r.restorationVerified&&r.packets.size()==11,"qualification flags");
    check(good.io.submissions==reports(good.b),"exact pulse sequence");check(good.io.clock==Time(88000),"pulse hold/spacing");
    for(unsigned i=0;i<11;++i)check(r.packets[i].submitted==Time(8000*i)&&r.packets[i].lateness==Time{}&&r.packets[i].spacing==Time(i?8000:0),"packet timing evidence");
    for(unsigned mask=0;mask<4;++mask){Harness h;h.io.rawResponseHook=[&](auto& f,auto& raw){if((f.writes==28&&(mask&1))||(f.writes==29&&(mask&2)))raw=anomaly;};const auto v=h.run();check(v.complete()&&!v.restorationVerified,"restore anomaly policy");for(unsigned i=0;i<2;++i)check(v.restoreReplies[i]==((mask&(1<<i))?ModeReply::CapturedZeroCountValue1:ModeReply::NormalSuccessAck),"restore classification");}
    for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved})for(unsigned i=1;i<=43;++i){Harness h;h.io.failAt=i;h.io.failStatus=status;auto v=h.run();check(!v.complete()&&h.io.writes==i,"write failure continued");check(v.deviceStateUncertain==(i>=15),"uncertain boundary");h.stopped();}
    for(unsigned i=1;i<=good.trace.count;++i){Harness h;h.trace.failAt=i;check(!h.run().complete(),"trace fault accepted");h.stopped();}
    for(unsigned i=0;i<43;++i){Harness h;h.control.cancelled=[&]{return h.io.writes>=i;};auto v=h.run();check(!v.complete()&&v.cancelled&&h.io.writes==i,"cancellation submitted another write");h.stopped();}
    for(unsigned i=1;i<=43;++i)if(i<16||i>26)for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved}){
        Harness h;h.io.readHook=[&](auto& f,Time,bool poll)->std::optional<IoResult>{if(!poll&&f.writes==i)return IoResult{status,{},0,123};return {};};
        check(!h.run().complete()&&h.io.writes==i,"read fault continued");h.stopped();
    }
    {Harness h;bool early=true;h.control.waitUntil=[&](Time due){h.io.clock=due-Time(early?1:0);early=!early;};check(h.run().complete(),"bounded early wake failed");}
    {Harness h;bool first=true;h.control.waitUntil=[&](Time due){h.io.clock=due+Time(first?2000:0);first=false;};auto v=h.run();check(v.complete()&&v.packets.back().lateness==Time(2000),"allowed fixed lateness rejected");}
    {Harness h;h.control.waitUntil=[&](Time due){h.io.clock=due+Time(1000);};auto v=h.run();check(!v.complete()&&h.io.writes==18,"accumulated drift caught after submission");h.stopped();}
    for(unsigned fault=0;fault<4;++fault){Harness h;bool cancelled=false;h.control.cancelled=[&]{return cancelled;};
        h.control.waitUntil=[&](Time due){h.io.clock=due;if(fault==0)h.trace.good=false;if(fault==1)h.io.present=false;if(fault==2)h.io.incoming.push_back(ack);if(fault==3)cancelled=true;};
        check(!h.run().complete()&&h.io.writes==16,"post-wait fault submitted a waveform");h.stopped();}
    for(unsigned write:{15u,27u,28u,29u})for(unsigned kind=0;kind<5;++kind){Harness h;h.io.rawResponseHook=[&](auto& f,auto& raw){if(f.writes!=write)return;if(kind==0)raw=anomaly;if(kind==1)raw.pop_back();if(kind==2){raw[6]=2;raw.back()+=2;}if(kind==3)raw.back()^=1;if(kind==4){raw[4]=2;raw.back()+=1;}};auto v=h.run();if(kind==0&&write>=28)check(v.complete(),"restore anomaly rejected");else {check(!v.complete()&&h.io.writes==write,"bad mode reply continued");h.stopped();}}
    // Changed preflight/postflight identity, firmware, profile, and RAM6.
    for(bool post:{false,true})for(auto cmd:{1,4,0xa1,0xa3}){Harness h;h.io.responseHook=[&](auto& f,auto c,auto& p){if(c==cmd&&(post?f.writes>29:f.writes<=14))p[cmd==1?10:0]^=1;};auto v=h.run();check(!v.complete()&&h.io.writes<=43,"changed baseline accepted");if(!post)check(v.modeWrites==0,"entry after baseline change");h.stopped();}
    for(unsigned scenario=0;scenario<10;++scenario){Harness h;
        h.io.readHook=[&](auto& f,Time,bool poll)->std::optional<IoResult>{
            if(scenario==0&&f.writes==14&&poll)return IoResult{Completion::Complete,ack,ack.size()};
            if(scenario==1&&f.writes==16&&poll)return IoResult{Completion::Complete,ack,ack.size()};
            if(scenario==2&&f.writes==15&&!poll){f.clock+=Time(600000);}
            if(scenario==3&&f.writes==15&&!poll){f.incoming.clear();}
            if(scenario==4&&f.writes==16&&poll){f.present=false;}
            if(scenario==5&&f.writes==15&&!poll){f.writeDelay=Time(4000);}
            if(scenario==6&&f.writes==15&&!poll){f.shortWrite=true;}
            return {};};
        if(scenario==7)h.control.waitUntil=[&](Time due){h.io.clock=due+Time(2001);};
        if(scenario==8)h.control.waitUntil=[](Time){};
        if(scenario==9)h.control.waitUntil=[&](Time){h.io.clock=Time(-1);};
        auto v=h.run();check(!v.complete(),"timing/drain/device fault accepted");h.stopped();}
    {Harness h;h.io.pretendPhysical=true;check(!h.run().complete()&&h.io.writes==0&&h.io.reads==0,"rehearsal opened physical");}
}
void timing() {
    PulseTiming t;rejects([&]{t.due(0);});t.entryReply(Time(100));rejects([&]{t.entryReply(Time(100));});rejects([&]{t.checkWave(0,Time(99));});rejects([&]{t.checkWave(0,Time(10101));});t.commitWave(0,Time(10100),Time(10101));
    rejects([&]{t.checkWave(1,Time(18099));});rejects([&]{t.checkWave(1,Time(20101));});t.commitWave(1,Time(20100),Time(20101));
    check(t.due(2)==Time(28100),"spacing must follow actual submission");rejects([&]{t.checkWave(2,Time(26100));});
    rejects([&]{t.commitWave(2,Time(28100),Time(32100));});
    for(unsigned i=2;i<11;++i)t.commitWave(i,Time(12100+8000*i),Time(12101+8000*i));
    check(t.exitDue()==Time(100101),"tail based on completion");rejects([&]{t.checkExit(Time(100100));});t.checkExit(Time(102101));rejects([&]{t.checkExit(Time(102102));});
}
struct Native {
    GripBaseline b=gripRehearsalFixture();std::vector<Bytes> wire=reports(b);PulseNativeGuard g{b,Time{}};Time clock{};
    void one(unsigned i){if(i>=15&&i<=25)clock=Time((i-15)*8000);if(i==26)clock=Time(88000);g.beforeWrite(wire[i],clock);g.afterWrite({Completion::Complete,{},33},clock,clock);if(i==14||i==26||i==27||i==28)g.afterRead({Completion::Complete,ack,33},clock);}
    void prefix(unsigned n){for(unsigned i=0;i<n;++i)one(i);}
};
void nativeGuard() {
    {Native n;n.prefix(43);check(n.g.nextIndex()==43,"native plan completion");rejects([&]{n.g.beforeWrite(n.wire[0],n.clock);});}
    // Every byte of every ordered report is independently guarded, including
    // configuration/trigger targets, amplitudes, report IDs, and padding.
    for(unsigned index=0;index<43;++index)for(unsigned byte=0;byte<33;++byte){Native n;n.prefix(index);auto bad=n.wire[index];bad[byte]^=1;rejects([&]{n.g.beforeWrite(bad,n.clock);});check(n.g.failed(),"native guard did not latch");rejects([&]{n.g.beforeWrite(n.wire[index],n.clock);});}
    for(unsigned index=0;index<43;++index){Native n;n.prefix(index);auto wrong=n.wire[(index+1)%43];if(wrong!=n.wire[index])rejects([&]{n.g.beforeWrite(wrong,n.clock);});}
    for(unsigned index:{14u,26u,27u,28u})for(bool anomalous:{false,true}){Native n;n.prefix(index);if(index>=26)n.clock=Time(88000);n.g.beforeWrite(n.wire[index],n.clock);n.g.afterWrite({Completion::Complete,{},33},n.clock,n.clock);auto raw=anomalous?anomaly:ack;if(anomalous&&index<27)rejects([&]{n.g.afterRead({Completion::Complete,raw,33},n.clock);});else n.g.afterRead({Completion::Complete,raw,33},n.clock);}
    {Native n;n.prefix(14);n.g.beforeWrite(n.wire[14],n.clock);n.g.afterWrite({Completion::Complete,{},33},n.clock,n.clock);rejects([&]{n.g.beforeWrite(n.wire[15],n.clock);});}
    {Native n;n.prefix(16);rejects([&]{n.g.afterRead({Completion::Complete,ack,33},n.clock);});}
    {Native n;n.prefix(17);const auto old=unhex("005aa5571b98808080808580808780808580808080807a80807880807a80000007");
     rejects([&]{n.g.beforeWrite(old,Time(16000));});check(n.g.failed(),"v2 packet accepted by v4 guard");}
    {Native n;n.prefix(17);std::array<StereoSample,8> oldSamples{};oldSamples[2][0]=.25;
     const auto old=n.b.binding.layout.wrap(gripWaveform(oldSamples));
     rejects([&]{n.g.beforeWrite(old,Time(16000));});check(n.g.failed(),"quarter-scale packet accepted by 12x guard");}
    {Native n;n.prefix(17);std::array<StereoSample,8> fullSamples{};fullSamples[2][0]=1;
     const auto full=n.b.binding.layout.wrap(gripWaveform(fullSamples));
     rejects([&]{n.g.beforeWrite(full,Time(16000));});check(n.g.failed(),"full-scale packet accepted by 12x guard");}
    for(auto t:{7999,10001}){Native n;n.prefix(16);rejects([&]{n.g.beforeWrite(n.wire[16],Time(t));});}
    {Native n;n.prefix(15);n.g.beforeWrite(n.wire[15],Time{});rejects([&]{n.g.afterWrite({Completion::Complete,{},33},Time{},Time(4000));});}
    {Native n;rejects([&]{n.g.checkTime(Time(75000000));});}
    {Native n;n.prefix(15);rejects([&]{n.g.checkTime(Time(5000000));});}
}
void approvals() {
    auto b=gripRehearsalFixture();b.physicalOrigin=true;b.binding.access=AccessMode::Shared;
    const std::string hash(64,'a');GripPulseApproval p{{hash,100,true,true,true,true,true,true,true,true,true,true,true,true,false},true,true};
    const auto encoded=encodeGripPulseApproval(p);check(encodeGripPulseApproval(decodeGripPulseApproval(encoded))==encoded,"pulse token round trip");
    auto a=GripPulseAuthorization::approve(b,p,hash,100);a.check(400);rejects([&]{a.check(401);});rejects([&]{a.check(99);});auto copy=a;a.consume();rejects([&]{copy.consume();});
    rejects([&]{GripPulseAuthorization::approve(b,p,std::string(64,'b'),100);});
    p.pulseAccepted=false;rejects([&]{GripPulseAuthorization::approve(b,p,hash,100);});p.pulseAccepted=true;p.silentRecoveryConfirmed=false;rejects([&]{GripPulseAuthorization::approve(b,p,hash,100);});
    rejects([&]{decodeGripPulseApproval(encodeGripRestoreApproval(p.checkpoints));});rejects([&]{decodeGripLifecycleApproval(encoded);});rejects([&]{decodeGripPulseApproval(encoded+"\n");});
    p.silentRecoveryConfirmed=true;b.physicalOrigin=false;rejects([&]{GripPulseAuthorization::approve(b,p,hash,100);});
}
}
int main(){try{successAndFaults();timing();nativeGuard();approvals();std::cout<<checks<<" pulse checks passed; zero HID opens\n";return 0;}catch(const std::exception& e){std::cerr<<"pulse check "<<checks<<": "<<e.what()<<'\n';return 1;}}
