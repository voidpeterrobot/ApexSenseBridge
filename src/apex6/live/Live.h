#pragma once
#include "apex6/live/Stream.h"
#include "apex6/experiment/Pulse.h"

namespace asb::apex6::live {
using namespace experiment;
#ifdef ASB_APEX6_LIVE_DEBUG_NO_DISPATCH_LATENESS
inline constexpr char scope[]="apex6-live-grips-debug-no-dispatch-lateness-v2";
inline constexpr bool enforceDispatchLateness=false;
inline constexpr char dispatchContract[]=";dispatch_late_us=disabled_debug";
#else
inline constexpr char scope[]="apex6-live-grips-v2";
inline constexpr bool enforceDispatchLateness=true;
inline constexpr char dispatchContract[]=";dispatch_late_us=2000";
#endif
struct Policy {
    unsigned seconds=60;
    double initialGain=1;
    bool strictDispatch=enforceDispatchLateness;
    bool continuous()const{return seconds==0;}
    void validate()const;
};
void validateIsolationLease(const std::string& text,const std::string& token,std::int64_t now);
struct Approval {GripRestoreApproval checkpoints;bool liveAccepted=false;};
std::string encodeApproval(const Approval&);
Approval decodeApproval(const std::string&);
class Authorization {
public:
    static Authorization approve(const GripBaseline&,Policy,const Approval&,const std::string& hash,std::int64_t now);
    void check(std::int64_t)const;
    void activate(std::int64_t)const;
    void consume()const;
    const GripBaseline& baseline()const{return baseline_;}
    Policy policy()const{return policy_;}
private:
    GripBaseline baseline_;Policy policy_;Approval approval_;std::string hash_;
    std::shared_ptr<std::atomic_bool> consumed_=std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic<std::int64_t>> activated_=std::make_shared<std::atomic<std::int64_t>>(-1);
};
// Used independently at the session and native submission boundaries. Only
// requestStop transitions streaming to a two-neutral-packet tail, irreversibly.
class NativeGuard {
public:
    NativeGuard(const GripBaseline&,Policy,Time start);
    void checkTime(Time);
    Time deadline(Time)const;
    void beforeWrite(std::span<const std::uint8_t>,Time);
    void afterWrite(const IoResult&,Time submitted,Time completed);
    void afterRead(const IoResult&,Time);
    void requestStop(Time);
    bool entryNext()const{return phase_==Phase::Entry;}
    bool complete()const{return phase_==Phase::Done;}
    Time due()const;
    Time streamEnd()const{return policy_.continuous()?Time::max()-Time(2000):entry_+Time(policy_.seconds*1000000LL);}
private:
    enum class Phase {Preflight,Entry,Lead,Stream,Tail,Exit,Left,Right,Postflight,Done};
    void advance();
    [[noreturn]] void fail(const char*);
    Layout layout_;Policy policy_;std::vector<Request> queries_,modes_;
    Phase phase_=Phase::Preflight;
    unsigned query_=0,tail_=0;
    std::uint64_t streamed_=0;
    bool failed_=false,pendingWrite_=false,pendingReply_=false;
    Time last_{},sessionEnd_{},entry_{},entryReply_{},lastWave_{},lastComplete_{},writeEnd_{},stopAt_{};
    bool haveWave_=false;
};
struct Control {
    std::function<void(Time)> waitUntil;
    std::function<bool()> cancelled;
    std::function<bool()> stopRequested;
    std::function<void()> pump; // processes raw records/controls outside native I/O
    std::function<Frame(Time)> packet;
    std::function<void()> discard;
    std::function<void(Time)> nativeStop;
    std::function<void()> progress; // main worker only; never callback/input thread
};
struct Result {
    bool strictDispatch=enforceDispatchLateness,integratedBeta=false;
    bool complete=false,postflightMatches=false,deviceStateUncertain=false;
    std::uint64_t queries=0,modes=0,waves=0,streamed=0;
    std::uint64_t lateDispatches=0,maxDispatchLatenessUs=0;
    std::string stopReason,failure;
    std::array<ModeReply,2> restores{ModeReply::NotObserved,ModeReply::NotObserved};
};
Result run(Io&,Trace&,const GripBaseline&,Policy,const Control&,const std::function<void()>& authorize={});
std::string resultJson(const Result&);
}
