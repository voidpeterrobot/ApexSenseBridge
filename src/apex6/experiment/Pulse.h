#pragma once
#include "apex6/experiment/Session.h"
#include "apex6/GripDsp.h"

namespace asb::apex6::experiment {
inline constexpr char gripPulseScope[]="grip-left-pulse-v4";
inline constexpr char gripPulseFixtureHash[]="3f3d5e06ae2c6acd956f3df9fbf9d6914cf7414d9423acfe3925b1cc171650ba";
inline constexpr double gripPulseBasePeak=1.0/16, gripPulseGain=12, gripPulsePeakLimit=.75;
std::array<Frame,11> gripPulseFrames(DspMetrics* strengthMetrics=nullptr);
std::vector<Request> gripPulsePlan(const GripBaseline&); // 15 actuator requests
struct GripPulseApproval {
    GripRestoreApproval checkpoints;
    bool pulseAccepted=false,silentRecoveryConfirmed=false;
};
std::string encodeGripPulseApproval(const GripPulseApproval&);
GripPulseApproval decodeGripPulseApproval(const std::string&);
class GripPulseAuthorization {
public:
    static GripPulseAuthorization approve(const GripBaseline&,const GripPulseApproval&,const std::string&,std::int64_t);
    const GripBaseline& baseline()const{return baseline_;}
    void check(std::int64_t)const;
    void consume()const;
private:
    GripBaseline baseline_;GripPulseApproval approval_;std::string hash_;
    std::shared_ptr<std::atomic_bool> consumed_=std::make_shared<std::atomic_bool>(false);
};
struct PulseControl {
    std::function<void(Time)> waitUntil; // may wake early; must be bounded/cancellable
    std::function<bool()> cancelled;
};
struct PulsePacketTiming {unsigned index;Time submitted,completed,nominal,lateness,spacing;};
struct PulseDispatchTiming {unsigned step;Time prepared,due,waitReturned{},checksComplete{};};
struct GripPulseResult {
    bool sequenceComplete=false,waveformComplete=false,postflightMatches=false;
    bool restorationVerified=false,deviceStateUncertain=false,cancelled=false;
    unsigned queryWrites=0,modeWrites=0,waveformWrites=0;
    std::array<ModeReply,2> restoreReplies{ModeReply::NotObserved,ModeReply::NotObserved};
    std::vector<PulsePacketTiming> packets;
    std::vector<PulseDispatchTiming> dispatches;
    std::string failure;
    bool complete()const{return sequenceComplete&&waveformComplete&&postflightMatches&&failure.empty()&&!cancelled;}
};
// Pure timing policy, used independently by session and native guard.
class PulseTiming {
public:
    void entryReply(Time);
    Time due(unsigned)const;
    void checkWave(unsigned,Time)const;
    PulsePacketTiming commitWave(unsigned,Time submitted,Time completed);
    Time exitDue()const;
    void checkExit(Time)const;
private:
    std::optional<Time> entry_,first_,last_,completed_;
    unsigned count_=0;
};
// Native transport owns its own guard. Never substitutes for session checks.
class PulseNativeGuard {
public:
    PulseNativeGuard(const GripBaseline&,Time start);
    void checkTime(Time);
    void beforeWrite(std::span<const std::uint8_t>,Time);
    void afterWrite(const IoResult&,Time submitted,Time completed);
    void afterRead(const IoResult&,Time);
    Time deadline(Time requested)const;
    bool failed()const{return failed_;}
    std::size_t nextIndex()const{return index_;}
private:
    [[noreturn]] void fail(const char*);
    Layout layout_;std::vector<Bytes> reports_;std::size_t index_=0;
    Time last_{},sessionEnd_{},activeEnd_{},writeDeadline_{};
    bool failed_=false,pendingWrite_=false;
    int pendingMode_=-1;
    PulseTiming timing_;
};
GripPulseResult rehearseGripPulse(Session&,const GripBaseline&,const PulseControl&);
GripPulseResult runAuthorizedGripPulse(Session&,const GripPulseAuthorization&,const std::function<std::int64_t()>&,const PulseControl&);
}
