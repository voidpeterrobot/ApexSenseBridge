#include "apex6/live/Live.h"
#include "apex6/experiment/Evidence.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace asb::apex6::live {
namespace {void require(bool ok,const char* why){if(!ok)throw ProtocolError(why);}}
void Policy::validate()const{require(seconds<=60&&std::isfinite(initialGain)&&initialGain>=0&&initialGain<=12,"invalid live policy");}
void validateIsolationLease(const std::string& text,const std::string& token,std::int64_t now){
    require(token.size()==32&&std::all_of(token.begin(),token.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"invalid isolation token");
    std::istringstream in(text);std::string actual;std::int64_t stamp=-1;in>>actual>>stamp;
    require(bool(in)&&actual==token&&stamp>=0&&stamp<=now&&now-stamp<=5,"isolation lease expired/mismatched");
    require(text==token+"\n"+std::to_string(stamp)+"\n","malformed isolation lease");
}
std::string encodeApproval(const Approval& a){return std::string("ASB_APEX6_LIVE_APPROVAL_V1\n")+scope+"\n"+(a.liveAccepted?"1\n":"0\n")+encodeGripRestoreApproval(a.checkpoints);}
Approval decodeApproval(const std::string& text) {
    const auto prefix=std::string("ASB_APEX6_LIVE_APPROVAL_V1\n")+scope+"\n1\n";
    require(text.size()<1400&&text.starts_with(prefix),"live approval scope mismatch");
    Approval a{decodeGripRestoreApproval(text.substr(prefix.size())),true};require(encodeApproval(a)==text,"noncanonical live approval");return a;
}
Authorization Authorization::approve(const GripBaseline& b,Policy p,const Approval& a,const std::string& hash,std::int64_t now){require(p.strictDispatch==enforceDispatchLateness,"experimental dispatch policy changed");Authorization r;r.baseline_=b;r.policy_=p;r.approval_=a;r.hash_=hash;r.check(now);return r;}
void Authorization::check(std::int64_t now)const{policy_.validate();gripLifecyclePlan(baseline_);require(approval_.liveAccepted,"live consent missing");const auto active=activated_->load();require(active<0||now>=active,"live authorization clock reversed");GripRestoreAuthorization::approve(baseline_,approval_.checkpoints,hash_,policy_.continuous()&&active>=0?active:now);}
void Authorization::activate(std::int64_t now)const{check(now);require(!consumed_->load(),"live authorization already consumed");if(policy_.continuous()){std::int64_t expected=-1;require(activated_->compare_exchange_strong(expected,now),"live session already activated");}}
void Authorization::consume()const{require(!consumed_->exchange(true),"live authorization already consumed");}
NativeGuard::NativeGuard(const GripBaseline& b,Policy p,Time start,ReplyBoundary boundary):layout_(b.binding.layout),policy_(p),queries_(gripBaselinePlan()),modes_(gripLifecyclePlan(b)),last_(start),boundary_(boundary),unit_(b.unit) {
    require(boundary==ReplyBoundary::None||(boundary==ReplyBoundary::DongleUidDiagnostic&&p.seconds==1&&!p.strictDispatch)||(boundary==ReplyBoundary::DongleLiveDiagnostic&&p.continuous()&&!p.strictDispatch),"invalid diagnostic reply boundary policy");
    p.validate();require(start.count()>=0&&start<Time::max()-Time(1200000000),"invalid live clock origin");sessionEnd_=boundary==ReplyBoundary::DongleLiveDiagnostic?start+Time(1200000000):p.continuous()?Time::max():start+Time(240000000);
}
[[noreturn]] void NativeGuard::fail(const char* why){failed_=true;throw ProtocolError(why);}
void NativeGuard::checkTime(Time t){if(failed_||t<last_||t>=sessionEnd_||(phase_==Phase::Stream&&t>streamEnd()+Time(2000))||(stopAt_.count()&&phase_!=Phase::Postflight&&phase_!=Phase::Done&&t>=stopAt_+Time(5000000)))fail("live native clock/deadline/failure");last_=t;}
Time NativeGuard::deadline(Time requested)const{auto d=std::min(requested,sessionEnd_);if(pendingWrite_||pendingReply_)d=std::min(d,writeEnd_);return d;}
Time NativeGuard::due()const {
    if(phase_==Phase::Exit)return lastComplete_+Time(8000);
    return haveWave_?lastWave_+Time(8000):entryReply_;
}
void NativeGuard::beforeWrite(std::span<const std::uint8_t> wire,Time t) {
    try {
        checkTime(t);require(!pendingWrite_&&!pendingReply_&&phase_!=Phase::Done,"live native write order");
        const bool wave=phase_==Phase::Lead||phase_==Phase::Stream||phase_==Phase::Tail;
        if(wave) {
            require(wire.size()==33&&wire[0]==0&&wire[3]==0x57&&wire[4]==27&&wire[5]==0x98,"live grip-only waveform framing");
            require(layout_.wrap(frame(0x57,wire.subspan(5,25)))==Bytes(wire.begin(),wire.end()),"live waveform checksum/padding");
            for(unsigned i=0;i<8;++i){require(wire[6+3*i]==128,"live trigger byte");for(unsigned c=1;c<=2;++c)require(wire[6+3*i+c]>=quantize(-.75)&&wire[6+3*i+c]<=quantize(.75),"live amplitude ceiling");}
            if(phase_!=Phase::Stream)require(layout_.wrap(gripWaveform({}))==Bytes(wire.begin(),wire.end()),"live lead/tail must be neutral");
            require(t>=due()&&(!policy_.strictDispatch||t<=due()+Time(2000)),"live dispatch early/late");
            if(phase_==Phase::Stream)require((policy_.continuous()||streamed_<7500)&&t<streamEnd(),"live streaming duration/budget");
        }else {
            const Request* r=nullptr;
            const Request boundaryQuery{4,{},16};
            if(boundaryPending_)r=&boundaryQuery;
            else if(phase_==Phase::Preflight||phase_==Phase::Postflight)r=&queries_.at(query_);
            else r=&modes_.at(phase_==Phase::Entry?0:phase_==Phase::Exit?1:phase_==Phase::Left?2:3);
            require(layout_.wrap(frame(r->command,r->payload))==Bytes(wire.begin(),wire.end()),"live ordered query/mode mismatch");
            if(phase_==Phase::Exit)require(t>=due()&&(!policy_.strictDispatch||t<=due()+Time(2000)),"live tail hold/exit deadline");
        }
        if(phase_==Phase::Entry)entry_=t;
        writeEnd_=t+Time(wave?4000:600000);pendingWrite_=true;
    }catch(...){failed_=true;throw;}
}
void NativeGuard::afterWrite(const IoResult& r,Time submitted,Time completed) {
    try {
        checkTime(completed);require(pendingWrite_&&r.status==Completion::Complete&&r.transferred==33&&completed>=submitted&&completed<writeEnd_,"live incomplete/late native write");pendingWrite_=false;
        if(phase_==Phase::Lead||phase_==Phase::Stream||phase_==Phase::Tail){haveWave_=true;lastWave_=submitted;lastComplete_=completed;if(phase_==Phase::Lead)phase_=Phase::Stream;else if(phase_==Phase::Stream)++streamed_;else if(++tail_==2){phase_=Phase::Exit;boundaryPending_=boundary_!=ReplyBoundary::None;}}
        else pendingReply_=true;
    }catch(...){failed_=true;throw;}
}
void NativeGuard::advance() {
    if(phase_==Phase::Preflight||phase_==Phase::Postflight){if(++query_==queries_.size()){query_=0;phase_=phase_==Phase::Preflight?Phase::Entry:Phase::Done;}}
    else if(phase_==Phase::Entry)phase_=Phase::Lead;
    else if(phase_==Phase::Exit)phase_=Phase::Left;
    else if(phase_==Phase::Left)phase_=Phase::Right;
    else if(phase_==Phase::Right)phase_=Phase::Postflight;
    boundaryPending_=boundary_!=ReplyBoundary::None&&(phase_==Phase::Left||phase_==Phase::Right);
}
void NativeGuard::afterRead(const IoResult& r,Time t) {
    try {
        checkTime(t);if(r.status==Completion::Idle&&r.bytes.empty())return;
        require(pendingReply_&&r.status==Completion::Complete&&r.transferred==33&&r.bytes.size()==33&&t<writeEnd_,"live unsolicited/incomplete/late native reply");
        const auto body=layout_.unwrap(r.bytes);
        if(boundaryPending_){const auto payload=replyPayload(body,4,16);require(payload.has_value()&&uid(*payload)==unit_,"dongle boundary UID mismatch");pendingReply_=false;boundaryPending_=false;return;}
        if(phase_==Phase::Preflight||phase_==Phase::Postflight){const auto& q=queries_[query_];require(replyPayload(body,q.command,q.replySize,q.v21).has_value(),"live query reply mismatch");}
        else {const auto c=classifyModeReply(body);require(c==ModeReply::NormalSuccessAck||((phase_==Phase::Left||phase_==Phase::Right)&&c==ModeReply::CapturedZeroCountValue1),"live mode reply rejected");}
        if(phase_==Phase::Entry)entryReply_=t;
        pendingReply_=false;advance();
    }catch(...){failed_=true;throw;}
}
void NativeGuard::requestStop(Time t){try{checkTime(t);require(phase_==Phase::Stream&&!pendingWrite_&&!pendingReply_,"live stop order");phase_=Phase::Tail;stopAt_=t;}catch(...){failed_=true;throw;}}

// A checked decorator lets baseline acquisition keep its existing parser while
// the same lifecycle validation covers both fake and physical sessions.
class CheckedIo final:public Io {
public:
    CheckedIo(Io& io,Trace& trace,NativeGuard& guard,const Control& c,Result& result,bool continuous):io(io),trace(trace),g(guard),c(c),result(result),end(continuous?Time::max():io.now()+Time(240000000)){}
    void check(){require(!failed,"live failure latched");try{if(c.progress)c.progress();require(!c.cancelled(),"live cancelled");require(trace.healthy(),"live trace failed");require(io.stillSameDevice(),"live physical device lost");require(io.now()<end,"live worker deadline");g.checkTime(io.now());}catch(...){failed=true;throw;}}
    Time now()const override{return io.now();}bool physical()const override{return io.physical();}const Binding& binding()const override{return io.binding();}bool stillSameDevice()override{check();return true;}
    IoResult read(Time d,bool poll)override{check();auto r=io.read(g.deadline(d),poll);g.afterRead(r,io.now());check();return r;}
    IoResult write(std::span<const std::uint8_t> bytes,Time d)override{
        check();const auto dispatch=io.now();
        if(bytes[3]==0x57&&(dispatch<g.due()||dispatch>g.due()+Time(2000))){
            const auto late=dispatch>g.due()?static_cast<std::uint64_t>((dispatch-g.due()).count()):0;
            if(late){++result.lateDispatches;result.maxDispatchLatenessUs=std::max(result.maxDispatchLatenessUs,late);}
            trace.record(dispatch,!enforceDispatchLateness&&late?"live_dispatch_late_debug":"live_dispatch_rejected",bytes,g.due().count(),late);
        }
        g.beforeWrite(bytes,dispatch);
        if(bytes[3]==0x53){++result.modes;result.deviceStateUncertain=true;}else if(bytes[3]==0x57){++result.waves;result.deviceStateUncertain=true;}else ++result.queries;
        auto r=io.write(bytes,g.deadline(d));require(r.submittedAt&&r.completedAt,"live native timestamps required");g.afterWrite(r,*r.submittedAt,*r.completedAt);check();return r;
    }
    Io& io;Trace& trace;NativeGuard& g;const Control& c;Result& result;Time end;bool failed=false;
};
Result run(Io& io,Trace& trace,const GripBaseline& baseline,Policy policy,const Control& c,const std::function<void()>& authorize,ReplyBoundary boundary) {
    Result result;result.strictDispatch=policy.strictDispatch;
    try {
        policy.validate();require(c.waitUntil&&c.cancelled&&c.stopRequested&&c.pump&&c.packet&&c.discard&&c.nativeStop,"live controls missing");
        require(!io.physical()||bool(authorize),"physical live requires authorization");
        NativeGuard guard(baseline,policy,io.now(),boundary);CheckedIo checked(io,trace,guard,c,result,policy.continuous());
        auto expected=baseline;if(!io.physical()){expected.physicalOrigin=false;expected.binding.access=AccessMode::Synthetic;}
        if(authorize)authorize();Session preflight(checked,trace);require(acquireGripBaseline(preflight)==expected,"live preflight changed");
        if(c.stopRequested()) {result.complete=true;result.stopReason="stopped_before_entry";return result;}
        const auto modes=gripLifecyclePlan(baseline);
        auto wait=[&](Time due){for(unsigned n=0;checked.now()<due;++n){checked.check();require(n<128,"live wait stalled");c.waitUntil(due);}checked.check();};
        auto send=[&](const Frame& f,bool mode,unsigned modeIndex=0){
            checked.check();const auto wire=io.binding().layout.wrap(f);trace.record(io.now(),"live_write_intent",wire);checked.check();
            auto drain=checked.read(io.now()+Time(600000),true);require(drain.status==Completion::Idle,"live stale input");
            if(!mode||modeIndex==1)wait(guard.due());
            if(mode&&modeIndex==0&&authorize)authorize();
            const auto deadline=io.now()+Time(mode?600000:4000);
            auto sent=checked.write(wire,deadline);trace.record(*sent.submittedAt,"live_native_submit",wire,result.waves);trace.record(*sent.completedAt,"live_native_complete",{},result.waves);checked.check();
            if(mode){auto reply=checked.read(deadline,false);const auto classification=classifyModeReply(io.binding().layout.unwrap(reply.bytes));trace.record(io.now(),std::string("live_mode_")+modeReplyName(classification),reply.bytes);if(modeIndex>=2)result.restores[modeIndex-2]=classification;}
        };
        c.discard();send(frame(modes[0].command,modes[0].payload),true,0);c.discard();send(gripWaveform({}),false);
        for(;;){
            checked.check();c.pump();checked.check();
            if(c.stopRequested()||guard.due()>=guard.streamEnd()||io.now()>=guard.streamEnd()||(!policy.continuous()&&result.streamed==7500)){result.stopReason=c.stopRequested()?"operator_q":"duration";break;}
            // Drain/DSP above while there is time before the next submission.
            // Never drain a raw batch after waiting until dispatch is due.
            // Packet selection still checks sample age at the dispatch boundary.
            wait(guard.due());checked.check();
            if(io.now()>=guard.streamEnd()||c.stopRequested()){result.stopReason=c.stopRequested()?"operator_q":"duration";break;}
            send(c.packet(io.now()),false);++result.streamed;
        }
        c.discard();guard.requestStop(io.now());c.nativeStop(io.now());
        send(gripWaveform({}),false);send(gripWaveform({}),false);
        for(unsigned i=1;i<4;++i){
            if(guard.boundaryNext()){
                if(i==1)wait(guard.due());
                Session separator(checked,trace);const Request query{4,{},16};
                separator.begin("dongle_shutdown_uid_boundary",Time(2000000),1,{query});
                require(uid(separator.exchange(query))==baseline.unit,"dongle shutdown UID changed");separator.end();
            }
            send(frame(modes[i].command,modes[i].payload),true,i);
        }
        Session postflight(checked,trace);require(acquireGripBaseline(postflight)==expected,"live postflight changed");result.postflightMatches=true;checked.check();require(guard.complete(),"live native lifecycle incomplete");result.complete=true;
    }catch(const std::exception& e){result.failure=e.what();result.stopReason="failure";}
    return result;
}
std::string resultJson(const Result& r){std::ostringstream o;o<<"{\"scope\":"<<json(r.integratedBeta?"apex6-integrated-beta-v1":scope)<<",\"dispatch_lateness_enforced\":"<<(r.strictDispatch?"true":"false")<<",\"late_dispatches\":"<<r.lateDispatches<<",\"max_dispatch_lateness_us\":"<<r.maxDispatchLatenessUs<<",\"complete\":"<<(r.complete?"true":"false")<<",\"postflight_matches\":"<<(r.postflightMatches?"true":"false")<<",\"restoration_verified\":false,\"device_state_uncertain\":"<<(r.deviceStateUncertain?"true":"false")<<",\"queries\":"<<r.queries<<",\"mode_writes\":"<<r.modes<<",\"waveform_writes\":"<<r.waves<<",\"streamed_packets\":"<<r.streamed<<",\"stop_reason\":"<<json(r.stopReason)<<",\"failure\":"<<json(r.failure)<<",\"restore_replies\":["<<json(modeReplyName(r.restores[0]))<<','<<json(modeReplyName(r.restores[1]))<<"]}\n";return o.str();}
}
