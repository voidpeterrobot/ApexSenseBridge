#include "apex6/dongle/Neutral.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6_rehearsal_fixture.h"
#include <iostream>
using namespace asb::apex6;
using namespace asb::apex6::experiment;
void check(bool ok, const char* why) { if (!ok) throw ProtocolError(why); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const ProtocolError&) { rejected=true; } check(rejected,"expected rejection"); }
struct Log : Trace {
    void record(Time,const std::string&,std::span<const std::uint8_t>,std::uint64_t,std::uint64_t) override {}
    bool healthy() const override { return true; }
};
struct Harness {
    GripBaseline baseline=gripRehearsalFixture(); FakeIo io{baseline}; Log trace;
    live::Control control{[&](Time t){io.clock=t;},[]{return false;},[]{return false;},[]{},
        [](Time){throw ProtocolError("caller packet must never be used");return Frame{};},[]{},[](Time){}};
    live::Result run(live::ReplyBoundary boundary=live::ReplyBoundary::None) { return dongle::runNeutral(io,trace,baseline,control,{},boundary); }
    live::Result pulse(dongle::PulseSide side=dongle::PulseSide::Left){return dongle::runPulse(io,trace,baseline,control,{},side);}
};
int main() { try {
    Harness h; const auto result=h.run();
    check(result.complete&&result.postflightMatches&&result.waves==11&&result.streamed==8&&result.modes==4&&result.queries==28,"neutral lifecycle budgets");
    check(h.io.writes==43&&result.stopReason=="neutral_packet_budget","neutral exact write count");
    for(unsigned channel=0;channel<3;++channel) {
        Harness bad; dongle::NeutralIo guard(bad.io);
        std::array<std::vector<double>,3> samples{}; samples[channel]={.1};
        auto wire=bad.baseline.binding.layout.wrap(waveform(samples,{channel==0,true,true}));
        rejects([&]{guard.write(wire,Time(1000000));});
        rejects([&]{guard.write(bad.baseline.binding.layout.wrap(gripWaveform({})),Time(1000000));});
        check(bad.io.writes==0,"rejected sample reached transport");
    }
    Harness budget; dongle::NeutralIo guard(budget.io); const auto neutral=budget.baseline.binding.layout.wrap(gripWaveform({}));
    for(unsigned i=0;i<11;++i)guard.write(neutral,Time(1000000));
    rejects([&]{guard.write(neutral,Time(1000000));});check(budget.io.writes==11,"excess waveform reached transport");
    for(unsigned index=1;index<=43;++index) {
        Harness failed; failed.io.failAt=index; check(!failed.run().complete&&failed.io.writes==index,"I/O failure did not stop traffic");
    }
    for(unsigned index=0;index<43;++index) {
        Harness cancelled;cancelled.control.cancelled=[&]{return cancelled.io.writes>=index;};
        check(!cancelled.run().complete&&cancelled.io.writes==index,"cancellation submitted cleanup");
    }
    Harness lost; lost.control.pump=[&]{lost.io.present=false;};check(!lost.run().complete&&lost.io.writes==16,"disconnect submitted cleanup");
    Harness exitReplyMissing;exitReplyMissing.io.rawResponseHook=[](auto& io,auto& wire){if(io.writes==27)wire.clear();};
    const auto missing=exitReplyMissing.run();check(!missing.complete&&!missing.postflightMatches&&missing.modes==2&&missing.waves==11&&exitReplyMissing.io.writes==27,"missing exit reply submitted restores/postflight");
    Harness anomalous; anomalous.io.rawResponseHook=[](auto& io,auto& wire){if(io.writes==28||io.writes==29)wire=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");};
    const auto observed=anomalous.run();check(observed.complete&&observed.restores[0]==ModeReply::CapturedZeroCountValue1&&observed.restores[1]==ModeReply::CapturedZeroCountValue1,"anomalous restores lost classification");
    const auto boundary=live::ReplyBoundary::DongleUidDiagnostic;
    {Harness separated;const auto r=separated.run(boundary);check(r.complete&&r.postflightMatches&&r.queries==31&&r.waves==11&&r.modes==4&&separated.io.writes==46,"UID boundary budgets");}
    for(unsigned index=1;index<=46;++index){Harness failed;failed.io.failAt=index;check(!failed.run(boundary).complete&&failed.io.writes==index,"boundary I/O failure submitted cleanup");}
    for(unsigned index=0;index<46;++index){Harness cancelled;cancelled.control.cancelled=[&]{return cancelled.io.writes>=index;};check(!cancelled.run(boundary).complete&&cancelled.io.writes==index,"boundary cancellation submitted cleanup");}
    for(unsigned index:{27u,29u,31u}){
        Harness wrong;wrong.io.responseHook=[&](auto& io,auto command,auto& payload){if(io.writes==index&&command==4)payload[0]^=1;};
        check(!wrong.run(boundary).complete&&wrong.io.writes==index,"wrong boundary identity allowed mode write");
        Harness missing;missing.io.rawResponseHook=[&](auto& io,auto& wire){if(io.writes==index)wire.clear();};
        check(!missing.run(boundary).complete&&missing.io.writes==index,"missing boundary reply allowed mode write");
    }
    {Harness missing;missing.io.rawResponseHook=[](auto& io,auto& wire){if(io.writes==28)wire.clear();};
        check(!missing.run(boundary).complete&&missing.io.writes==28,"boundary variant ignored missing exit reply");}
    // A synthetic receiver that suppresses identical consecutive responses:
    // the original path must fail, while UID boundaries must preserve all ACKs.
    for(auto variant:{live::ReplyBoundary::None,boundary}){
        Harness repeated;Bytes previous;
        repeated.io.rawResponseHook=[&](auto&,auto& wire){if(wire==previous)wire.clear();else previous=wire;};
        const auto r=repeated.run(variant);check(r.complete==(variant==boundary),"duplicate-response model outcome");
    }
    {Harness anomaly;anomaly.io.rawResponseHook=[](auto& io,auto& wire){if(io.writes==30||io.writes==32)wire=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");};
        const auto r=anomaly.run(boundary);check(r.complete&&r.restores[0]==ModeReply::CapturedZeroCountValue1&&r.restores[1]==ModeReply::CapturedZeroCountValue1,"boundary anomaly classification lost");}
    {Harness pulse;const auto r=pulse.pulse();check(r.complete&&r.postflightMatches&&r.queries==31&&r.modes==4&&r.waves==35&&r.streamed==32&&pulse.io.writes==70,"pulse lifecycle budget");
        unsigned waves=0;for(const auto& wire:pulse.io.submissions)if(wire[3]==0x57){
            check(dongle::allowedPulseWave(wire,waves++),"pulse pattern mismatch");
            for(unsigned s=0;s<8;++s){check(wire[6+3*s]==128&&wire[8+3*s]==128,"pulse enabled trigger/right");check(wire[7+3*s]>=quantize(-.0625)&&wire[7+3*s]<=quantize(.0625),"pulse amplitude exceeded");}}
    }
    for(unsigned index=1;index<=70;++index){Harness failed;failed.io.failAt=index;check(!failed.pulse().complete&&failed.io.writes==index,"pulse fault cleanup");}
    for(unsigned index=0;index<70;++index){Harness cancelled;cancelled.control.cancelled=[&]{return cancelled.io.writes>=index;};check(!cancelled.pulse().complete&&cancelled.io.writes==index,"pulse cancellation cleanup");}
    {Harness early;early.control.stopRequested=[&]{return early.io.writes>=20;};const auto r=early.pulse();check(r.complete&&r.postflightMatches&&r.stopReason=="operator_q"&&r.waves<35,"pulse orderly early stop");}
    {Harness malformed;dongle::BoundedIo guard(malformed.io,boundary,dongle::PulseSide::Left);auto wire=malformed.baseline.binding.layout.wrap(dongle::pulsePacket(0));rejects([&]{guard.write(wire,Time(1000000));});check(malformed.io.writes==0,"nonneutral pulse lead escaped");}
    for(unsigned column=0;column<3;++column){Harness bad;dongle::BoundedIo guard(bad.io,boundary,dongle::PulseSide::Left);guard.write(bad.baseline.binding.layout.wrap(gripWaveform({})),Time(1000000));std::array<std::vector<double>,3> samples;samples[column]={.125};rejects([&]{guard.write(bad.baseline.binding.layout.wrap(waveform(samples,{true,true,true})),Time(1000000));});check(bad.io.writes==1,"noncanonical pulse escaped");}
    {Harness missing;missing.io.rawResponseHook=[](auto& io,auto& wire){if(io.writes==52)wire.clear();};const auto r=missing.pulse();check(!r.complete&&!r.postflightMatches&&missing.io.writes==52,"pulse missing exit reply ignored");}
    for(auto side:{dongle::PulseSide::Right,dongle::PulseSide::Both}){
        Harness pulse;const auto r=pulse.pulse(side);check(r.complete&&r.postflightMatches&&r.waves==35&&pulse.io.writes==70,"selected pulse budget");
        unsigned index=0;bool left=false,right=false;
        for(const auto& wire:pulse.io.submissions)if(wire[3]==0x57){check(dongle::allowedPulseWave(wire,index++,side),"selected pattern mismatch");for(unsigned s=0;s<8;++s){check(wire[6+3*s]==128,"selected pulse trigger");left|=wire[7+3*s]!=128;right|=wire[8+3*s]!=128;for(unsigned c:{7u,8u})check(wire[c+3*s]>=quantize(-.0625)&&wire[c+3*s]<=quantize(.0625),"selected pulse amplitude");}}
        check(left==(side==dongle::PulseSide::Both)&&right,"selected pulse channel mapping");
        for(unsigned i=0;i<32;++i){const auto actual=dongle::pulsePacket(i,side),l=dongle::pulsePacket(i,dongle::PulseSide::Left),r=dongle::pulsePacket(i,dongle::PulseSide::Right);for(unsigned s=0;s<8;++s){check(actual[6+3*s]==(side==dongle::PulseSide::Both?l[6+3*s]:128)&&actual[7+3*s]==r[7+3*s],"both composition");}}
        for(unsigned index=1;index<=70;++index){Harness failed;failed.io.failAt=index;check(!failed.pulse(side).complete&&failed.io.writes==index,"selected pulse fault cleanup");}
        for(unsigned index=0;index<70;++index){Harness cancelled;cancelled.control.cancelled=[&]{return cancelled.io.writes>=index;};check(!cancelled.pulse(side).complete&&cancelled.io.writes==index,"selected pulse cancellation cleanup");}
        Harness wrong;dongle::BoundedIo guard(wrong.io,boundary,side);guard.write(wrong.baseline.binding.layout.wrap(gripWaveform({})),Time(1000000));rejects([&]{guard.write(wrong.baseline.binding.layout.wrap(dongle::pulsePacket(0,dongle::PulseSide::Left)),Time(1000000));});check(wrong.io.writes==1,"wrong side escaped wrapper");
    }
    rejects([]{dongle::pulsePacket(0,dongle::PulseSide::None);});
    rejects([]{dongle::pulsePacket(0,static_cast<dongle::PulseSide>(999));});
    for(auto side:{dongle::PulseSide::None,static_cast<dongle::PulseSide>(999)}){Harness h;rejects([&]{h.pulse(side);});check(h.io.writes==0,"invalid side reached transport");}
    {Harness h;h.control.packet=[](Time){return dongle::pulsePacket(0);};
        const auto r=live::run(h.io,h.trace,h.baseline,{0,1,false},h.control,{},live::ReplyBoundary::DongleLiveDiagnostic);
        check(r.complete&&r.postflightMatches&&r.stopReason=="duration"&&r.queries==31&&r.modes==4&&r.streamed>74000&&r.streamed<=75000,"dongle live ten-minute lifecycle");
        check(h.io.clock>=Time(600000000)&&h.io.clock<Time(601000000),"dongle live active bound");}
    {Harness h;h.control.packet=[](Time){return dongle::pulsePacket(0);};h.control.stopRequested=[&]{return h.io.writes>=20;};
        const auto r=live::run(h.io,h.trace,h.baseline,{0,1,false},h.control,{},live::ReplyBoundary::DongleLiveDiagnostic);
        check(r.complete&&r.postflightMatches&&r.stopReason=="operator_q"&&r.queries==31,"dongle live early stop with boundaries");}
    {Harness h;live::NativeGuard guard(h.baseline,{0,1,false},Time{},live::ReplyBoundary::DongleLiveDiagnostic);rejects([&]{guard.checkTime(Time(1200000000));});}
    {Harness h;rejects([&]{live::NativeGuard guard(h.baseline,{1,1,false},Time{},live::ReplyBoundary::DongleLiveDiagnostic);});}
    std::cout<<"Dongle neutral/pulse budgets, waveform rejection, write faults, cancellation and restore classifications passed\n"; return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
