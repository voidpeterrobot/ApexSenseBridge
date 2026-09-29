#include "apex6/live/Live.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6_rehearsal_fixture.h"
#include "apex6_live_fixture.h"
#include <iostream>
#include <thread>
#include <cmath>
#include <algorithm>
#include <fstream>
using namespace asb::apex6;
using namespace asb::apex6::experiment;
namespace live=asb::apex6::live;
void check(bool ok,const char* why){if(!ok)throw ProtocolError(why);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,"expected rejection");}
struct Log:Trace{bool good=true;unsigned count=0,failAt=0;void record(Time,const std::string&,std::span<const std::uint8_t>,std::uint64_t,std::uint64_t)override{if(++count==failAt)good=false;}bool healthy()const override{return good;}};
struct Harness {
    GripBaseline baseline=gripRehearsalFixture();FakeIo io{baseline};Log log;live::Stream stream;
    std::uint64_t sequence=0;bool stop=false,cancel=false;Time previous{-1};
    live::Control control{[&](Time t){io.clock=t;},[&]{return cancel;},[&]{return stop;},[&]{if(previous!=io.clock){stream.ingest({livePcm(++sequence),io.clock},io.clock);previous=io.clock;}},[&](Time t){return stream.packet(t);},[&]{stream.reset();},[](Time){}};
    live::Result run(unsigned seconds=1){return live::run(io,log,baseline,{seconds,1},control);}
};
void streamTests(){
    live::GainRamp ramp;DspMetrics metrics;ramp.target(0);
    for(unsigned i=1;i<=100;++i){ramp.apply({},metrics);check(std::abs(ramp.current()-(1-i/100.))<1e-12,"linear mute ramp");}
    ramp.target(12);for(unsigned i=0;i<50;++i)ramp.apply({},metrics);check(ramp.current()==6,"half ramp");ramp.target(1);ramp.apply({},metrics);check(std::abs(ramp.current()-5.95)<1e-12,"ramp retarget continuity");
    for(unsigned i=0;i<99;++i)ramp.apply({},metrics);check(ramp.current()==1,"ramp target");
    live::GainRamp saturate(12);auto s=saturate.apply({1,-1},metrics);check(s[0]==.75&&s[1]==-.75&&metrics.clippedSamples==2&&metrics.peakBeforeLimit==12,"signed ceiling metrics");
    rejects([&]{ramp.target(13);});rejects([&]{ramp.target(NAN);});
    for(unsigned channel=0;channel<4;++channel){live::Stream stream;Frame f{};for(unsigned i=1;i<=4;++i){stream.ingest({livePcm(i,1,channel,32),Time(i*8000)},Time(i*8000));f=stream.packet(Time(i*8000));}bool left=false,right=false;for(unsigned i=0;i<8;++i){left|=f[6+3*i]!=128;right|=f[7+3*i]!=128;check(f[5+3*i]==128,"neutral triggers");}check(left==(channel==2)&&right==(channel==3),"channel separation/padding");}
    live::Stream stream;stream.ingest({livePcm(1),Time{}},Time{});stream.ingest({livePcm(2,2,3),Time(8000)},Time(8000));auto f=stream.packet(Time(8000));for(unsigned i=0;i<8;++i)check(f[6+3*i]==128,"generation retained old left channel");
    check(stream.packet(Time(60000))==gripWaveform({}),"stale audio played");check(stream.metrics().underrunSamples>=8,"underruns not counted");
    check(!stream.ingest({livePcm(3,2),Time{}},Time(50000)),"stale record accepted");rejects([&]{stream.ingest({livePcm(4,1),Time(60000)},Time(60000));});
    StereoResampler fragmented(4,1,1,false),whole(4,1,1,false);auto raw=livePcm(1);auto pcm=std::span(raw).subspan(96);auto expected=whole.feed(pcm);std::vector<StereoSample> actual;for(auto byte:pcm){auto next=fragmented.feed(std::span(&byte,1));actual.insert(actual.end(),next.begin(),next.end());}fragmented.finish();check(actual==expected,"fragmented PCM changed FIR");
    for(unsigned i=4;i<20;++i)stream.ingest({livePcm(i,2),Time(60000)},Time(60000));check(stream.metrics().peakSamples<=40&&stream.metrics().droppedSamples>0,"processed backlog bound");
}
void queueTests(){
    live::RawQueue q([]{return Time{};});auto raw=livePcm(1);const auto saved=raw;q.submit(raw);raw.assign(raw.size(),0);check(q.pop()->bytes==saved,"callback bytes not owned");
    q.submit(livePcm(3));check(q.failed(),"sequence loss accepted");
    live::RawQueue overflow;for(unsigned i=1;i<2000&&!overflow.failed();++i)overflow.submit(livePcm(i));check(overflow.failed()&&overflow.metrics().peakBytes<=4*1024*1024,"raw queue unbounded");
    live::RawQueue bad;auto invalid=livePcm(1);invalid[92]=1;bad.submit(invalid);check(bad.failed(),"failed packet accepted");
    live::RawQueue malformed;malformed.submit(Bytes(80));check(malformed.failed(),"malformed accepted");
    live::RawQueue concurrent;std::thread producer([&]{for(unsigned i=1;i<=100;++i)concurrent.submit(livePcm(i));});while(producer.joinable()){if(concurrent.metrics().records==100)break;concurrent.pop();std::this_thread::yield();}producer.join();check(!concurrent.failed(),"queue shutdown race");
}
void hidTests(){
    const auto nonzero=[](const Frame& f,unsigned c){for(unsigned i=0;i<8;++i)if(f[6+3*i+c]!=128)return true;return false;};
    for(bool control:{false,true})for(unsigned c:{0u,1u}){
        live::Stream s;check(s.ingest({liveHid(1,c==0?255:0,c==1?255:0,control),Time{}},Time{}),"HID rumble not ready");
        s.discardPcm();auto f=s.packet(Time(8000));check(nonzero(f,c)&&!nonzero(f,1-c),"HID motor routing/pre-entry state");
        for(unsigned i=0;i<8;++i)check(f[5+3*i]==128,"HID synth drove trigger");
        s.ingest({liveHid(2,0,0,control),Time(9000)},Time(9000));check(s.packet(Time(9000))==gripWaveform({}),"explicit HID zero did not stop");
    }
    {live::Stream s;s.ingest({liveHid(1,255,255,false,2,4),Time{}},Time{});check(nonzero(s.packet(Time{}),0),"improved compatible rumble ignored");}
    for(unsigned size:{63u,64u}){live::Stream s;auto padded=liveHid(1);padded.resize(80+size);padded[8]=static_cast<std::uint8_t>(padded.size());padded[60]=static_cast<std::uint8_t>(size);check(s.ingest({padded,Time{}},Time{}),"padded USB rumble ignored");}
    {live::Stream s;s.ingest({liveHid(1),Time{}},Time{});s.packet(Time(1999999));check(s.packet(Time(2000000))==gripWaveform({})&&s.metrics().hidTimeouts==1,"HID stale lease replayed");}
    {live::Stream s;auto led=liveHid(1,255,255,false,0);check(!s.ingest({led,Time{}},Time{})&&s.packet(Time{})==gripWaveform({}),"unflagged motor bytes accepted");}
    {live::Stream s;s.ingest({liveHid(1),Time{}},Time{});s.ingest({liveHid(2,255,255,false,1),Time(8000)},Time(8000));check(s.packet(Time(8000))==gripWaveform({}),"haptics-select release ignored");}
    {live::Stream s;check(!s.ingest({liveHid(1),Time{}},Time(40001)),"stale HID started output");}
    {live::Stream s;auto wrong=liveHid(1,255,255,true);wrong[84]=2;check(!s.ingest({wrong,Time{}},Time{}),"audio-interface SET_REPORT accepted");}
    {live::Stream s;auto wrong=liveHid(1,255,255,true);wrong[86]=47;rejects([&]{s.ingest({wrong,Time{}},Time{});});}
    {live::Stream s;auto wrong=liveHid(1);wrong.pop_back();wrong[8]=static_cast<std::uint8_t>(wrong.size());wrong[60]=47;rejects([&]{s.ingest({wrong,Time{}},Time{});});}
    {live::Stream s;auto wrong=liveHid(1);wrong[40]=2;rejects([&]{s.ingest({wrong,Time{}},Time{});});}
    {live::Stream s;s.ingest({liveHid(1),Time{}},Time{});s.ingest({liveHid(2,0,0,false,0,0,2),Time(8000)},Time(8000));check(s.packet(Time(8000))==gripWaveform({}),"generation replayed HID strength");}
    {live::Stream s;s.ingest({liveHid(1,255,0),Time{}},Time{});s.ingest({livePcm(2,1,3),Time(8000)},Time(8000));auto f=s.packet(Time(8000));check(!nonzero(f,0)&&nonzero(f,1),"PCM did not take priority");check(!s.ingest({liveHid(3),Time(16000)},Time(16000)),"HID accepted over active PCM");check(s.packet(Time(120000))==gripWaveform({}),"suppressed HID replayed after PCM");check(s.ingest({liveHid(4),Time(128000)},Time(128000)),"fresh HID cannot resume after PCM");}
    {live::Stream s;s.ingest({livePcm(1),Time{}},Time{});s.packet(Time(50000));check(!s.ingest({liveHid(2),Time(60000)},Time(60000)),"stale PCM queue prematurely ended priority hold");}
    {live::Stream s;auto quiet=livePcm(1);std::fill(quiet.begin()+96,quiet.end(),0);s.ingest({quiet,Time{}},Time{});check(s.ingest({liveHid(2),Time(8000)},Time(8000)),"silent endpoint blocked HID");check(nonzero(s.packet(Time(8000)),0),"silent PCM masked rumble");check(nonzero(s.packet(Time(50000)),0),"stale silent PCM erased leased HID");}
    {live::Stream s(12);s.ingest({liveHid(1,255,255),Time{}},Time{});s.packet(Time{});check(s.metrics().strength.clippedSamples>0&&s.metrics().strength.peakAfterLimit<=.75,"HID bypassed gain/ceiling");s.gain(0);for(unsigned i=0;i<13;++i)s.packet(Time(i*8000));check(s.packet(Time(104000))==gripWaveform({}),"HID mute ramp failed");}
    const std::string token(32,'a');live::validateIsolationLease(token+"\n1000\n",token,1005);
    rejects([&]{live::validateIsolationLease(token+"\n1000\n",token,1006);});
    rejects([&]{live::validateIsolationLease(token+"\n1001\n",token,1000);});
    rejects([&]{live::validateIsolationLease(token+"\n1000\n",std::string(32,'b'),1000);});
    rejects([&]{live::validateIsolationLease(token+"\n1000\njunk",token,1000);});
}
void lifecycleTests(){
    Harness h;auto r=h.run(60);check(r.complete,r.failure.c_str());check(r.postflightMatches&&r.modes==4&&r.queries==28&&r.streamed==7499&&r.waves==7502,"60-second command budgets");check(h.io.clock==Time(60016000),"physical timer/tail hold");
    Harness hid;hid.control.pump=[&]{if(hid.previous!=hid.io.clock){hid.stream.ingest({liveHid(++hid.sequence,200,100),hid.io.clock},hid.io.clock);hid.previous=hid.io.clock;}};auto hidRun=hid.run(60);check(hidRun.complete&&hidRun.postflightMatches&&hidRun.streamed==7499&&hid.stream.metrics().hidSamples>50000,"60-second HID-only lifecycle failed");
    Harness drift;drift.control.waitUntil=[&](Time due){drift.io.clock=due+Time(1000);};check(drift.run().complete,"per-dispatch drift incorrectly cumulative");
    Harness dspCost;auto pump=dspCost.control.pump;dspCost.control.pump=[&]{pump();dspCost.io.clock+=Time(3000);};check(dspCost.run().complete,"bounded DSP work spent dispatch lateness allowance");
    Harness excessiveDsp;excessiveDsp.control.pump=[&]{excessiveDsp.io.clock+=Time(10001);};auto costly=excessiveDsp.run();check(costly.complete==!live::enforceDispatchLateness&&(!live::enforceDispatchLateness||excessiveDsp.io.writes==16),"processing overrun dispatch policy");
    Harness late;late.control.waitUntil=[&](Time due){late.io.clock=due+Time(11017);};late.control.stopRequested=[&]{return late.io.clock>=Time(100000);};auto delayed=late.run(0);check(delayed.complete==!live::enforceDispatchLateness&&delayed.lateDispatches>0&&delayed.maxDispatchLatenessUs==11017&&(!live::enforceDispatchLateness||late.io.writes==16),"late waveform dispatch policy/evidence");
    for(unsigned stopAt:{0u,15u,16u,25u,140u}){Harness fault;fault.control.cancelled=[&]{return fault.io.writes>=stopAt;};check(!fault.run().complete&&fault.io.writes==stopAt,"cancellation continued traffic");}
    for(unsigned failAt:{1u,14u,15u,16u,50u,143u}){Harness fault;fault.io.failAt=failAt;check(!fault.run().complete&&fault.io.writes==failAt,"I/O failure continued traffic");}
    Harness disconnect;disconnect.control.pump=[&]{disconnect.io.present=false;};check(!disconnect.run().complete&&disconnect.io.writes==16,"disconnect continued traffic");
    Harness trace;trace.log.failAt=10;check(!trace.run().complete,"trace failure accepted");
    Harness orderly;orderly.control.stopRequested=[&]{return orderly.io.writes>=20;};auto q=orderly.run();check(q.complete&&q.stopReason=="operator_q"&&q.modes==4,"Q skipped orderly tail");
    Harness changed;changed.io.snapshot.config.crc16[0]^=1;check(!changed.run().complete&&changed.io.writes==14,"changed baseline entered");
    Harness continuous;continuous.control.stopRequested=[&]{return continuous.io.clock>=Time(300000000);};auto longRun=continuous.run(0);check(longRun.complete&&longRun.postflightMatches&&longRun.stopReason=="operator_q"&&longRun.streamed==37499&&longRun.waves==37502&&longRun.modes==4&&longRun.queries==28,"continuous session stopped at old duration/budget or skipped tail");
    Harness continuousCancel;continuousCancel.control.cancelled=[&]{return continuousCancel.io.clock>=Time(70000000);};auto cancelled=continuousCancel.run(0);check(!cancelled.complete&&cancelled.streamed>7500&&cancelled.modes==1,"continuous cancellation issued cleanup or stopped early");
    Harness continuousLate;continuousLate.control.waitUntil=[&](Time due){continuousLate.io.clock=due+Time(2001);};continuousLate.control.stopRequested=[&]{return continuousLate.io.clock>=Time(100000);};check(continuousLate.run(0).complete==!live::enforceDispatchLateness&&(!live::enforceDispatchLateness||continuousLate.io.writes==16),"continuous dispatch policy");
}
void nativeTests(){
    auto b=gripRehearsalFixture();auto setup=[&](live::NativeGuard& guard,FakeIo& io){for(const auto& req:gripBaselinePlan()){auto wire=b.binding.layout.wrap(frame(req.command,req.payload));guard.beforeWrite(wire,io.now());auto w=io.write(wire,Time(1000000));guard.afterWrite(w,*w.submittedAt,*w.completedAt);auto r=io.read(Time(1000000),false);guard.afterRead(r,io.now());}auto req=gripLifecyclePlan(b)[0];auto wire=b.binding.layout.wrap(frame(req.command,req.payload));guard.beforeWrite(wire,io.now());auto w=io.write(wire,Time(1000000));guard.afterWrite(w,*w.submittedAt,*w.completedAt);guard.afterRead(io.read(Time(1000000),false),io.now());};
    for(unsigned fault=0;fault<7;++fault){live::NativeGuard guard(b,{1,1},Time{});FakeIo io(b);setup(guard,io);auto wave=gripWaveform({});if(fault==0)wave=frame(0x53,Bytes{1,0x12,0});if(fault==1)wave[4]|=4;if(fault==2)wave[5]=129;if(fault==3)wave[6]=255;if(fault==4)wave[31]^=1;if(fault==5)wave=frame(0xa1,{});if(fault==6)io.clock=Time(2001);if(fault==6&&!live::enforceDispatchLateness)guard.beforeWrite(b.binding.layout.wrap(wave),io.now());else rejects([&]{guard.beforeWrite(b.binding.layout.wrap(wave),io.now());});rejects([&]{guard.beforeWrite(b.binding.layout.wrap(gripWaveform({})),io.now());});}
    live::NativeGuard order(b,{},Time{});rejects([&]{order.beforeWrite(b.binding.layout.wrap(frame(0x53,Bytes{1,0x12,2,0x40,0})),Time{});});
    rejects([&]{live::Policy{61,1}.validate();});rejects([&]{live::decodeApproval(encodeGripPulseApproval({}));});
    live::Approval approval;auto encoded=live::encodeApproval(approval);const std::string otherScope=live::enforceDispatchLateness?"apex6-live-grips-debug-no-dispatch-lateness-v1":"apex6-live-grips-v1";encoded.replace(encoded.find(live::scope),std::string(live::scope).size(),otherScope);rejects([&]{live::decodeApproval(encoded);});
}
int replay(const char* path){
    std::ifstream file(path,std::ios::binary);check(bool(file),"raw replay open failed");
    Bytes bytes((std::istreambuf_iterator<char>(file)),{});check(bytes.size()<=64*1024*1024,"replay exceeds bound");
    live::Stream stream;Time now{};live::RawQueue queue([&]{return now;});std::size_t offset=0,packets=0;
    while(offset<bytes.size()){
        check(bytes.size()-offset>=80,"truncated replay header");auto record=std::span(bytes).subspan(offset);
        auto size=asb::capture::u32(record,8);check(size>=80&&size<=record.size(),"truncated replay record");record=record.first(size);
        now=Time(asb::capture::u64(record,32)/1000);queue.submit(record,true);check(!queue.failed(),"replay callback validation failed");
        stream.ingest(*queue.pop(),now);if(stream.packet(now)!=gripWaveform({}))++packets;offset+=size;
    }
    const auto& m=stream.metrics();std::cout<<"{\"records\":"<<queue.metrics().records<<",\"hid_accepted\":"<<m.hidAccepted<<",\"hid_ignored\":"<<m.hidIgnored<<",\"hid_suppressed\":"<<m.hidSuppressed<<",\"hid_samples\":"<<m.hidSamples<<",\"nonneutral_packets\":"<<packets<<"}\n";
    check(m.hidAccepted>0&&m.hidSamples>0&&packets>0,"captured HID commands produced no output");return 0;
}
int main(int argc,char** argv){try{if(argc==2)return replay(argv[1]);streamTests();queueTests();hidTests();lifecycleTests();nativeTests();std::cout<<"Live stream, HID arbitration, isolation lease, 60-second simulation and fault checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
