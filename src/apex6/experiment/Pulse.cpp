#include "apex6/experiment/Pulse.h"
#include <algorithm>

namespace asb::apex6::experiment {
namespace {void require(bool b,const char* s){if(!b)throw ProtocolError(s);}}
std::array<Frame,11> gripPulseFrames(DspMetrics* strengthMetrics) {
    constexpr std::array<double,8> cycle{0,.7071067811865476,1,.7071067811865476,0,-.7071067811865476,-1,-.7071067811865476};
    std::array<Frame,11> result{};
    const GripStrength strength(gripPulseGain,gripPulsePeakLimit);DspMetrics metrics;
    for(unsigned p=0;p<result.size();++p) {
        std::array<std::array<double,2>,8> samples{};
        if(p>0&&p<9)for(unsigned i=0;i<8;++i)samples[i][0]=cycle[i]*gripPulseBasePeak*((p==1||p==8)?.5:1);
        for(auto& sample:samples)sample=strength.apply(sample,metrics);
        result[p]=gripWaveform(samples);
    }
    if(strengthMetrics)*strengthMetrics=metrics;return result;
}
std::vector<Request> gripPulsePlan(const GripBaseline& b) {
    const auto modes=gripLifecyclePlan(b);std::vector<Request> result{modes[0]};
    for(const auto& f:gripPulseFrames())result.push_back({0x57,Bytes(f.begin()+4,f.begin()+29),0});
    result.insert(result.end(),modes.begin()+1,modes.end());return result;
}
std::string encodeGripPulseApproval(const GripPulseApproval& a) {
    return std::string("ASB_APEX6_GRIP_PULSE_APPROVAL_V1\n")+gripPulseScope+"\n"+(a.pulseAccepted?"1 ":"0 ")+(a.silentRecoveryConfirmed?"1\n":"0\n")+encodeGripRestoreApproval(a.checkpoints);
}
GripPulseApproval decodeGripPulseApproval(const std::string& text) {
    const auto prefix=std::string("ASB_APEX6_GRIP_PULSE_APPROVAL_V1\n")+gripPulseScope+"\n";
    require(text.size()<1400&&text.starts_with(prefix)&&text.size()>=prefix.size()+4,"pulse approval scope mismatch");
    const auto flags=text.substr(prefix.size(),4);require(flags=="1 1\n"||flags=="1 0\n"||flags=="0 1\n"||flags=="0 0\n","pulse approval flags invalid");
    GripPulseApproval a{decodeGripRestoreApproval(text.substr(prefix.size()+4)),flags[0]=='1',flags[2]=='1'};
    require(!a.checkpoints.observeRepliesAccepted&&encodeGripPulseApproval(a)==text,"noncanonical pulse approval");return a;
}
GripPulseAuthorization GripPulseAuthorization::approve(const GripBaseline& b,const GripPulseApproval& p,const std::string& h,std::int64_t now) {
    gripPulsePlan(b);GripPulseAuthorization a;a.baseline_=b;a.approval_=p;a.hash_=h;a.check(now);return a;
}
void GripPulseAuthorization::check(std::int64_t now)const {
    require(approval_.pulseAccepted&&approval_.silentRecoveryConfirmed,"pulse/recovery confirmation missing");
    GripRestoreAuthorization::approve(baseline_,approval_.checkpoints,hash_,now,false);
}
void GripPulseAuthorization::consume()const {require(!consumed_->exchange(true),"pulse authorization already consumed");}
void PulseTiming::entryReply(Time t){require(!entry_&&t.count()>=0,"duplicate/invalid pulse entry reply");entry_=t;}
Time PulseTiming::due(unsigned k)const {
    require(k==count_&&k<11&&entry_.has_value(),"pulse timing index/entry mismatch");
    if(!first_)return *entry_;
    return std::max(*first_+Time(8000*k),*last_+Time(8000));
}
void PulseTiming::checkWave(unsigned k,Time t)const {
    require(t>=due(k),"early pulse submission");
    require(first_?t<=*first_+Time(8000*k+2000):t<=*entry_+Time(10000),"late pulse submission");
}
PulsePacketTiming PulseTiming::commitWave(unsigned k,Time s,Time c) {
    checkWave(k,s);require(c>=s&&c<s+Time(4000),"late/reversed pulse completion");
    if(!first_)first_=s;
    PulsePacketTiming r{k,s,c,*first_+Time(8000*k),s-(*first_+Time(8000*k)),last_?s-*last_:Time{}};
    last_=s;completed_=c;++count_;return r;
}
Time PulseTiming::exitDue()const {require(count_==11&&completed_.has_value(),"pulse tail incomplete");return *completed_+Time(8000);}
void PulseTiming::checkExit(Time t)const {
    require(t>=exitDue()&&t<=exitDue()+Time(2000)&&t<*first_+Time(100000),"pulse exit timing missed");
}
PulseNativeGuard::PulseNativeGuard(const GripBaseline& b,Time start):layout_(b.binding.layout),last_(start) {
    require(start.count()>=0&&start<=Time::max()-Time(75000000),"invalid pulse clock origin");sessionEnd_=start+Time(75000000);
    for(const auto& phase:{gripBaselinePlan(),gripPulsePlan(b),gripBaselinePlan()})for(const auto& r:phase)reports_.push_back(layout_.wrap(frame(r.command,r.payload)));
}
[[noreturn]] void PulseNativeGuard::fail(const char* s){failed_=true;throw ProtocolError(s);}
void PulseNativeGuard::checkTime(Time t) {
    if(failed_||t<last_||t>=sessionEnd_||(activeEnd_.count()&&index_<=29&&t>=activeEnd_))fail("native pulse clock/deadline/failure guard");last_=t;
}
Time PulseNativeGuard::deadline(Time requested)const {
    auto d=std::min(requested,sessionEnd_);
    if(activeEnd_.count()&&index_<=29)d=std::min(d,activeEnd_);
    if(pendingWrite_||pendingMode_>=0)d=std::min(d,writeDeadline_);
    return d;
}
void PulseNativeGuard::beforeWrite(std::span<const std::uint8_t> wire,Time t) {
    try {
        checkTime(t);require(!pendingWrite_&&pendingMode_<0&&index_<reports_.size()&&Bytes(wire.begin(),wire.end())==reports_[index_],"native pulse ordered report/response mismatch");
        if(index_==14)activeEnd_=t+Time(5000000);
        if(index_>=15&&index_<=25)timing_.checkWave(static_cast<unsigned>(index_-15),t);
        if(index_==26)timing_.checkExit(t);
        writeDeadline_=t+Time(index_>=15&&index_<=25?4000:600000);pendingWrite_=true;
    }catch(...){failed_=true;throw;}
}
void PulseNativeGuard::afterWrite(const IoResult& r,Time s,Time c) {
    try {
        checkTime(c);require(pendingWrite_&&r.status==Completion::Complete&&r.transferred==33&&c<deadline(writeDeadline_),"native pulse incomplete/late write");
        if(index_>=15&&index_<=25)timing_.commitWave(static_cast<unsigned>(index_-15),s,c);
        if(index_==14||index_==26||index_==27||index_==28)pendingMode_=static_cast<int>(index_);
        pendingWrite_=false;++index_;
    }catch(...){failed_=true;throw;}
}
void PulseNativeGuard::afterRead(const IoResult& r,Time t) {
    try {
        checkTime(t);
        if(r.status==Completion::Idle&&r.bytes.empty())return;
        require(r.status==Completion::Complete&&r.transferred==r.bytes.size(),"native pulse read fault");
        if(pendingMode_>=0) {
            require(t<writeDeadline_,"native pulse late mode reply");const auto c=classifyModeReply(layout_.unwrap(r.bytes));
            require(c==ModeReply::NormalSuccessAck||((pendingMode_==27||pendingMode_==28)&&c==ModeReply::CapturedZeroCountValue1),"native pulse mode reply rejected");
            if(pendingMode_==14)timing_.entryReply(t);pendingMode_=-1;
        } else if(index_>=15&&index_<=26)fail("native pulse unsolicited waveform input");
    }catch(...){failed_=true;throw;}
}

class GripPulseRunner {
public:
    GripPulseRunner(Session& session,const PulseControl& control):t(session),control(control){}
    GripPulseResult run(const GripBaseline& baseline,const std::function<void()>& authorize) {
        try {
            require(control.cancelled&&control.waitUntil,"pulse requires cancellation and bounded wait callbacks");
            require(t.queries_==0&&t.actuators_==0&&!t.scopedGuard_,"pulse requires a fresh session");
            const auto active=gripPulsePlan(baseline);
            result.packets.reserve(11);result.dispatches.reserve(12);
            t.scopedGuard_=[&]{if(control.cancelled()){result.cancelled=true;t.stop("pulse cancelled; no cleanup");}if(!t.io_.stillSameDevice())t.stop("pulse device lost");};
            auto expected=baseline;if(!t.physical()){expected.physicalOrigin=false;expected.binding.access=AccessMode::Synthetic;}
            if(authorize)authorize();require(acquireGripBaseline(t)==expected,"fresh pulse baseline differs");
            t.begin("grip_left_pulse",Time(5000000),15,active);
            for(unsigned i=0;i<active.size();++i)exchange(active[i],i,authorize);
            t.end();result.sequenceComplete=true;
            require(acquireGripBaseline(t)==expected,"pulse postflight changed");t.guard();result.postflightMatches=true;
        }catch(const std::exception& e){if(!t.failed()){try{t.stop(e.what());}catch(...){}}result.failure=t.failure();}
        t.scopedGuard_={};result.deviceStateUncertain=t.uncertain();result.queryWrites=t.queryAttempts();return result;
    }
private:
    Time wait(Time due) {
        auto returned=t.now();
        for(unsigned n=0;;++n) {
            t.guard();if(t.now()>=due)return returned;require(n<128,"pulse wait wake limit");
            control.waitUntil(due);returned=t.now();
        }
    }
    void exchange(const Request& r,unsigned step,const std::function<void()>& authorize) {
        t.guard();require(t.index_==step&&t.plan_[step]==r&&t.attempts_<t.budget_,"pulse ordered request mismatch");
        // Prepare and persist intent before waiting. Keep the final presence,
        // cancellation, trace, input-drain and native timing checks after it.
        const auto wire=t.binding_.layout.wrap(frame(r.command,r.payload));
        ++t.operation_;t.trace_.record(t.now(),"write_attempt",wire,t.operation_);t.guard();
        if(step>=1&&step<=12) {
            const auto due=step==12?timing.exitDue():timing.due(step-1);
            result.dispatches.push_back({step,t.now(),due});
            result.dispatches.back().waitReturned=wait(due);
        }
        // Waveform phase and pre-exit drain reject every unsolicited report.
        for(unsigned n=0;;++n) {
            t.guard();require(n<64,"pulse drain limit");auto d=t.io_.read(t.deadline_,true);
            if(!d.bytes.empty())t.trace_.record(t.now(),"drain",d.bytes,t.operation_);t.guard();
            if(d.status==Completion::Idle&&d.bytes.empty())break;
            require(d.status==Completion::Complete&&d.transferred==d.bytes.size(),"pulse drain failed");
            if(step>=1&&step<=12)t.stop("pulse unsolicited waveform input");
            auto body=t.binding_.layout.unwrap(d.bytes);require(body.size()<3||body[0]!=0x5a||body[1]!=0xa5||body[2]!=r.command,"pulse stale same-opcode reply");
        }
        if(step==0&&authorize){authorize();t.guard();}
        const auto started=t.now();
        if(step>=1&&step<=12)result.dispatches.back().checksComplete=started;
        if(r.command==0x57)timing.checkWave(step-1,started);
        if(step==12)timing.checkExit(started);
        const auto deadline=std::min(t.deadline_,started+Time(r.command==0x57?4000:600000));
        ++t.attempts_;++t.index_;++t.actuators_;t.uncertain_=true;
        if(r.command==0x57)++result.waveformWrites;else ++result.modeWrites;
        auto w=t.io_.write(wire,deadline);
        t.trace_.record(t.now(),"write_return",{},t.operation_,w.transferred);
        t.trace_.record(t.now(),"write_status",{},t.operation_,(static_cast<std::uint64_t>(w.status)<<32)|w.error);t.guard();
        require(w.status==Completion::Complete&&w.transferred==wire.size()&&t.now()<deadline,"pulse write incomplete or late; no retry");
        if(r.command==0x57) {
            require(w.submittedAt&&w.completedAt&&*w.submittedAt>=started&&*w.completedAt<=t.now(),"pulse native write timestamps missing/invalid");
            const auto p=timing.commitWave(step-1,*w.submittedAt,*w.completedAt);result.packets.push_back(p);
            t.trace_.record(p.submitted,"pulse_native_submit",wire,t.operation_,p.lateness.count());
            t.trace_.record(p.completed,"pulse_native_complete",{},t.operation_,p.spacing.count());t.guard();
            result.waveformComplete=result.packets.size()==11;return;
        }
        auto rx=t.io_.read(deadline,false);
        t.trace_.record(t.now(),"receive",rx.bytes,t.operation_,rx.error);
        t.trace_.record(t.now(),"receive_status",{},t.operation_,(static_cast<std::uint64_t>(rx.status)<<32)|rx.transferred);t.guard();
        require(rx.status==Completion::Complete&&rx.transferred==rx.bytes.size()&&t.now()<deadline,"pulse reply missing/incomplete/late");
        ModeReply c=ModeReply::InvalidUnexpected;
        try{c=classifyModeReply(t.binding_.layout.unwrap(rx.bytes));}catch(const ProtocolError&){}
        if(step==13||step==14)result.restoreReplies[step-13]=c;
        t.trace_.record(t.now(),std::string("mode_reply_")+modeReplyName(c),rx.bytes,t.operation_);t.guard();
        require(c==ModeReply::NormalSuccessAck||((step==13||step==14)&&c==ModeReply::CapturedZeroCountValue1),"pulse mode reply rejected");
        if(step==0)timing.entryReply(t.now());
    }
    Session& t;const PulseControl& control;PulseTiming timing;GripPulseResult result;
};
GripPulseResult rehearseGripPulse(Session& t,const GripBaseline& b,const PulseControl& c) {
    if(t.physical()){GripPulseResult r;r.failure="physical pulse requires separate approval";return r;}
    return GripPulseRunner(t,c).run(b,{});
}
GripPulseResult runAuthorizedGripPulse(Session& t,const GripPulseAuthorization& a,const std::function<std::int64_t()>& clock,const PulseControl& c) {
    if(!t.physical()||t.binding()!=a.baseline().binding){GripPulseResult r;r.failure="pulse physical binding mismatch";return r;}
    return GripPulseRunner(t,c).run(a.baseline(),[&]{a.check(clock());});
}
}
