#include "apex6/live/Live.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6_rehearsal_fixture.h"
#include "apex6_live_fixture.h"
#include "core/ControllerCapabilities.h"
#include "platform/Apex6Settings.h"
#include <iostream>
#include <limits>
using namespace asb::apex6;
using namespace asb::apex6::experiment;
namespace live=asb::apex6::live;
void check(bool ok,const char* message){if(!ok)throw ProtocolError(message);}
bool active(const Frame& f){return f!=gripWaveform({});}
struct TraceCounter:Trace {std::uint64_t count=0;void record(Time,const std::string&,std::span<const std::uint8_t>,std::uint64_t,std::uint64_t)override{++count;}bool healthy()const override{return true;}};
void heldMotor(){
    live::Stream waiting(0,live::Stream::HidLifetime::UntilChanged);
    waiting.ingest({liveHid(1,0,0),Time{}},Time{});
    check(!waiting.eligible(Time{}),"explicit zero entered muted session");
    auto quiet=livePcm(2);std::fill(quiet.begin()+96,quiet.end(),0);waiting.ingest({quiet,Time{}},Time{});
    check(!waiting.eligible(Time{}),"silence entered grip mode");
    waiting.ingest({liveHid(3,255,0),Time{}},Time{});check(waiting.eligible(Time{}),"muted nonzero feedback did not qualify");
    live::Stream s(1,live::Stream::HidLifetime::UntilChanged);
    s.ingest({liveHid(1,255,255),Time{}},Time{});
    check(active(s.packet(Time(3000000))),"HID expired after two seconds");
    auto silent=livePcm(2);std::fill(silent.begin()+96,silent.end(),0);
    s.ingest({silent,Time(3010000)},Time(3010000));check(active(s.packet(Time(3010000))),"silent PCM cancelled HID");
    s.ingest({livePcm(3),Time(3020000)},Time(3100000));check(active(s.packet(Time(3100000))),"stale PCM cancelled HID");
    s.ingest({liveHid(4,0,0),Time(3100000)},Time(3150000));check(active(s.packet(Time(3150000))),"stale HID cancelled accepted state");
    s.ingest({livePcm(5),Time(3200000)},Time(3200000));s.packet(Time(3200000));
    check(!active(s.packet(Time(3350000))),"PCM takeover retained earlier HID");
    s.ingest({liveHid(6),Time(3360000)},Time(3360000));check(active(s.packet(Time(3360000))),"fresh HID could not resume");
    s.gain(0);for(unsigned i=0;i<14;++i)s.packet(Time(3370000+i*8000));check(!active(s.packet(Time(3500000))),"mute failed");
    s.gain(1);for(unsigned i=0;i<14;++i)s.packet(Time(3510000+i*8000));check(active(s.packet(Time(3640000))),"gain reset lost held state");
    s.ingest({liveHid(7,0,0),Time(3650000)},Time(3650000));check(!active(s.packet(Time(3650000))),"explicit zero ignored");
    s.ingest({liveHid(8),Time(3660000)},Time(3660000));s.ingest({liveHid(9,0,0,false,0,0,2),Time(3670000)},Time(3670000));check(!active(s.packet(Time(3670000))),"generation retained HID");
    s.ingest({liveHid(10,255,255,false,3,0,2),Time(3680000)},Time(3680000));s.reset();check(!active(s.packet(Time(3690000))),"stop retained HID");
    check(!asb::platform::validGripGain(std::numeric_limits<double>::infinity())&&!asb::platform::validGripGain(-1)&&asb::platform::validGripGain(12),"gain validation");
}
void hourSimulation(){
    auto b=gripRehearsalFixture();FakeIo io(b);TraceCounter trace;
    live::Stream stream(1,live::Stream::HidLifetime::UntilChanged);live::RawQueue queue([&]{return io.clock;});
    std::uint64_t sequence=0,tick=0;auto pcm=livePcm(1);
    auto stamp=[&](Bytes& bytes){++sequence;for(unsigned i=0;i<8;++i){bytes[16+i]=static_cast<std::uint8_t>(sequence>>(8*i));bytes[32+i]=static_cast<std::uint8_t>((sequence*8000000)>>(8*i));}};
    live::Control control;
    control.waitUntil=[&](Time due){io.clock=due+(tick%113==0?Time(3200):Time{});};
    control.cancelled=[]{return false;};control.stopRequested=[&]{return io.clock>=Time(3600000000LL);};
    control.pump=[&]{
        ++tick;io.submissions.clear();const auto phase=(io.clock.count()/1000000)%30;
        if(phase<10){if(tick%500==1){auto hid=liveHid(1,200,100);stamp(hid);queue.submit(hid);}}
        else if(phase<20){stamp(pcm);queue.submit(pcm);}
        else if(tick%500==1){auto hid=liveHid(1,100,200);stamp(hid);queue.submit(hid);}
        while(auto item=queue.pop())stream.ingest(*item,io.clock);
        if(tick%10000==0)stream.gain((tick/10000)%2?12:1);
    };
    control.packet=[&](Time now){return stream.packet(now);};control.discard=[&]{stream.discardPcm();};control.nativeStop=[](Time){};
    auto result=live::run(io,trace,b,{0,1,false},control);
    check(result.complete&&result.postflightMatches,"one-hour lifecycle failed");
    check(result.streamed>400000&&result.lateDispatches>0,"continuous duration/lateness policy");
    check(stream.metrics().hidSamples>0&&stream.metrics().validRecords>0,"mixed feedback missing");
    check(stream.metrics().peakSamples<=40&&queue.metrics().peakBytes<10000&&!queue.failed(),"memory bound");
    check(result.restores[0]!=ModeReply::NotObserved&&result.deviceStateUncertain,"restore was silently verified");
    std::cout<<"3600 simulated seconds: "<<result.streamed<<" packets, "<<result.lateDispatches<<" late dispatches; peak queue "<<queue.metrics().peakBytes<<" bytes; PCM peak "<<stream.metrics().peakSamples<<" samples\n";
}
int main(){try{heldMotor();hourSimulation();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
