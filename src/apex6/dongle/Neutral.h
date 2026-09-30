#pragma once
#include "apex6/live/Live.h"
#include "apex6/dongle/Pulse.h"

namespace asb::apex6::dongle {
inline constexpr char scope[] = "apex6-dongle-neutral-v1";
inline constexpr char boundaryScope[] = "apex6-dongle-neutral-uid-boundaries-v1";
inline constexpr unsigned maximumWaves = 11;

// A second, diagnostic-specific boundary immediately before native submission.
// The underlying integrated native guard still enforces lifecycle ordering,
// minimum spacing, native deadlines, reply validation and exclusive access.
class BoundedIo final : public experiment::Io {
public:
    explicit BoundedIo(experiment::Io& io,live::ReplyBoundary boundary=live::ReplyBoundary::None,PulseSide side=PulseSide::None) : io_(io),maximumAttempts_(side!=PulseSide::None?70:boundary==live::ReplyBoundary::DongleUidDiagnostic?46:43),side_(side) {
        if(side!=PulseSide::None&&(!validPulseSide(side)||boundary!=live::ReplyBoundary::DongleUidDiagnostic))reject("dongle pulse requires valid side and UID boundaries");
    }
    experiment::Time now() const override { return io_.now(); }
    bool physical() const override { return io_.physical(); }
    const experiment::Binding& binding() const override { return io_.binding(); }
    bool stillSameDevice() override { return !failed_ && io_.stillSameDevice(); }
    experiment::IoResult read(experiment::Time deadline, bool poll) override {
        check(); return io_.read(deadline, poll);
    }
    experiment::IoResult write(std::span<const std::uint8_t> wire,
                               experiment::Time deadline) override {
        check();
        if (wire.size() != 33 || attempts_ >= maximumAttempts_) reject("dongle neutral framing/write budget");
        if (wire[3] == 0x57) {
            if (side_!=PulseSide::None?!allowedPulseWave(wire,waves_,side_):(waves_ >= maximumWaves ||
                Bytes(wire.begin(), wire.end()) != binding().layout.wrap(gripWaveform({}))))
                reject("dongle rejects unexpected waveform or excess packets");
            ++waves_;
        }
        ++attempts_;
        auto result = io_.write(wire, deadline);
        if (result.status != experiment::Completion::Complete || result.transferred != wire.size()) failed_ = true;
        return result;
    }
    unsigned waves() const { return waves_; }
private:
    [[noreturn]] void reject(const char* reason) { failed_ = true; throw ProtocolError(reason); }
    void check() { if (failed_) reject("dongle neutral failure latched"); }
    experiment::Io& io_;
    unsigned waves_ = 0, attempts_ = 0;
    unsigned maximumAttempts_;
    PulseSide side_;
    bool failed_ = false;
};
using NeutralIo=BoundedIo;

inline live::Result runNeutral(experiment::Io& native, experiment::Trace& trace,
    const experiment::GripBaseline& baseline, live::Control control,
    const std::function<void()>& authorize = {},live::ReplyBoundary boundary=live::ReplyBoundary::None) {
    NeutralIo guarded(native,boundary);
    const auto stop = control.stopRequested;
    control.stopRequested = [&] { return guarded.waves() >= 9 || (stop && stop()); };
    control.packet = [](experiment::Time) { return gripWaveform({}); };
    auto result = live::run(guarded, trace, baseline, {1, 1, false}, control, authorize,boundary);
    if (result.complete && result.waves == maximumWaves) result.stopReason = "neutral_packet_budget";
    return result;
}

inline live::Result runPulse(experiment::Io& native,experiment::Trace& trace,
    const experiment::GripBaseline& baseline,live::Control control,
    const std::function<void()>& authorize={},PulseSide side=PulseSide::Left) {
    if(!validPulseSide(side))throw ProtocolError("invalid dongle pulse selection");
    BoundedIo guarded(native,live::ReplyBoundary::DongleUidDiagnostic,side);
    const auto stop=control.stopRequested;
    control.stopRequested=[&]{return guarded.waves()>=pulsePackets+1||(stop&&stop());};
    control.packet=[&](experiment::Time){return pulsePacket(guarded.waves()-1,side);};
    auto result=live::run(guarded,trace,baseline,{1,1,false},control,authorize,live::ReplyBoundary::DongleUidDiagnostic);
    if(result.complete&&result.waves==pulseMaximumWaves)result.stopReason=std::string(pulseName(side))+"_pulse_packet_budget";
    return result;
}
}
