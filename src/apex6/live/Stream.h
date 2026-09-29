#pragma once
#include "capture/RawFeedbackSink.h"
#include "capture/RawCapture.h"
#include "apex6/GripDsp.h"
#include "apex6/experiment/Session.h"
#include <mutex>

namespace asb::apex6::live {
using experiment::Time;
Time monotonic();
struct RawItem { Bytes bytes; Time received; bool terminal=false; };
struct QueueMetrics { std::uint64_t records=0,rejected=0; std::size_t peakBytes=0; };
class RawQueue final : public capture::RawFeedbackSink {
public:
    explicit RawQueue(std::function<Time()> clock=monotonic):clock_(std::move(clock)){}
    void submit(std::span<const std::uint8_t>,bool terminal=false) noexcept override;
    void fail(const char*) noexcept override;
    bool failed()const noexcept override { return failure_.load()!=nullptr; }
    const char* failure()const noexcept {return failure_.load();}
    std::optional<RawItem> pop();
    QueueMetrics metrics()const;
private:
    mutable std::mutex mutex_;
    std::deque<RawItem> queue_;
    std::size_t bytes_=0;
    std::uint64_t sequence_=0,generation_=0,timestamp_=0;
    QueueMetrics metrics_;
    std::atomic<const char*> failure_{nullptr};
    std::function<Time()> clock_;
};
class GainRamp {
public:
    explicit GainRamp(double initial=1);
    void target(double);
    StereoSample apply(StereoSample,DspMetrics&);
    double current()const {return current_;}
    double target()const {return target_;}
private:
    double current_,start_,target_; unsigned remaining_=0;
};
struct StreamMetrics {
    std::uint64_t staleRecords=0,droppedSamples=0,underrunSamples=0,resets=0,validRecords=0;
    std::uint64_t hidAccepted=0,hidIgnored=0,hidSuppressed=0,hidTimeouts=0,hidSamples=0;
    std::size_t peakSamples=0;
    DspMetrics strength;
};
class Stream {
public:
    enum class HidLifetime { DiagnosticLease, UntilChanged };
    enum class Source { AwaitingFeedback, Pcm, Hid, Muted };
    explicit Stream(double initial=1,HidLifetime lifetime=HidLifetime::DiagnosticLease):gain_(initial),lifetime_(lifetime){}
    // True for valid PCM or an accepted, nonzero HID motor update. Requested
    // submission ABI's authoritative lengths; trailing URB padding is excluded.
    bool ingest(const RawItem&,Time now);
    void reset();
    // Discard pre-entry PCM, retaining only a still-leased HID state command.
    void discardPcm();
    void gain(double value){gain_.target(value);}
    Frame packet(Time now);
    const StreamMetrics& metrics()const {return metrics_;}
    double gain()const {return gain_.current();}
    Source source(Time now)const;
    bool eligible(Time now)const {return pcmOwns(now)||(rumbleAt_&&(rumble_[0]!=0||rumble_[1]!=0));}
private:
    bool ingestHid(const capture::RecordView&,Time received,Time now);
    bool pcmOwns(Time now)const;
    struct Sample {StereoSample value;Time received;};
    StereoResampler resampler_{4,1,1,false};
    GainRamp gain_;
    std::deque<Sample> samples_;
    std::uint64_t generation_=0,timestamp_=0,lastAudioTimestamp_=0;
    std::optional<Time> lastAudio_;
    std::optional<Time> activePcm_,rumbleAt_;
    StereoSample rumble_{};
    std::uint64_t rumblePhase_=0;
    StreamMetrics metrics_;
    HidLifetime lifetime_;
};
}
