// SPDX-FileCopyrightText: 2026 Mikalai Kaliaha
// SPDX-License-Identifier: MIT
#pragma once
#include "apex6/Gpa6.h"
#include <chrono>
#include <deque>

namespace asb::apex6 {
using StereoSample = std::array<double, 2>;
struct DspMetrics {
    std::uint64_t inputFrames = 0, outputFrames = 0, overrangeSamples = 0, resets = 0;
    std::uint64_t clippedSamples = 0;
    double peakBeforeLimit = 0, peakAfterLimit = 0;
};
// Shared fixed-strength stage for resampled PCM and bounded diagnostic tones.
class GripStrength {
public:
    explicit GripStrength(double gain = 1.0, double peakLimit = 1.0);
    StereoSample apply(StereoSample sample, DspMetrics& metrics) const;
private:
    double gain_, peakLimit_;
};
class StereoResampler {
public:
    // Fixed software strength for this stream; not calibrated motor force.
    explicit StereoResampler(unsigned channels = 4, double gain = 1.0, double peakLimit = 1.0, bool applyStrength = true);
    // Explicit reset on generation/format changes or stale gaps, never mixes histories.
    void reset();
    std::vector<StereoSample> feed(std::span<const std::uint8_t> pcm);
    void finish() const; // rejects incomplete input frame; does not flush FIR tail
    const DspMetrics& metrics() const { return metrics_; }
    std::size_t pendingBytes() const { return pendingSize_; }
private:
    unsigned channels_;
    bool applyStrength_;
    GripStrength strength_;
    std::array<double, 769> taps_{};
    std::array<StereoSample, 769> history_{};
    std::array<std::uint8_t, 8> pending_{};
    std::size_t head_ = 0, pendingSize_ = 0;
    unsigned phase_ = 0;
    DspMetrics metrics_{};
};

// Clock-driven OFFLINE queue model. It does not sleep, own a device or perform
// lifecycle queries. Caller supplies monotonic completion times, not wall time.
class PacketScheduler {
public:
    using Time = std::chrono::microseconds;
    explicit PacketScheduler(unsigned maxLatencyMs = 40);
    void reset(Time now);
    void enqueue(std::span<const StereoSample> samples, Time now);
    std::optional<Frame> tick(Time now);
    std::uint64_t dropped() const { return dropped_; }
    std::uint64_t underruns() const { return underruns_; }
    std::size_t queued() const { return queue_.size(); }
private:
    struct Item { StereoSample sample; Time arrived; };
    void checkTime(Time now);
    std::deque<Item> queue_;
    std::size_t capacity_;
    Time latency_, next_{}, last_{};
    std::uint64_t dropped_ = 0, underruns_ = 0;
};
}
