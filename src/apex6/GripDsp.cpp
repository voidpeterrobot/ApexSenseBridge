// SPDX-FileCopyrightText: 2026 Mikalai Kaliaha
// SPDX-License-Identifier: MIT
#include "apex6/GripDsp.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace asb::apex6 {
GripStrength::GripStrength(double gain, double peakLimit) : gain_(gain), peakLimit_(peakLimit) {
    if (!std::isfinite(gain) || gain < 0 || gain > 12)
        throw ProtocolError("expected finite gain in [0,12]");
    if (!std::isfinite(peakLimit) || peakLimit < 0 || peakLimit > 1)
        throw ProtocolError("expected finite peak limit in [0,1]");
}
StereoSample GripStrength::apply(StereoSample sample, DspMetrics& metrics) const {
    for (auto value : sample)
        if (!std::isfinite(value) || !std::isfinite(value * gain_))
            throw ProtocolError("nonfinite strength sample");
    for (auto& value : sample) {
        value *= gain_;
        if (std::abs(value) > 1) ++metrics.overrangeSamples;
        metrics.peakBeforeLimit = std::max(metrics.peakBeforeLimit, std::abs(value));
        if (std::abs(value) > peakLimit_) ++metrics.clippedSamples;
        value = std::clamp(value, -peakLimit_, peakLimit_);
        metrics.peakAfterLimit = std::max(metrics.peakAfterLimit, std::abs(value));
    }
    return sample;
}
StereoResampler::StereoResampler(unsigned channels, double gain, double peakLimit, bool applyStrength)
    : channels_(channels), applyStrength_(applyStrength), strength_(gain, peakLimit) {
    if (channels != 2 && channels != 4) throw ProtocolError("expected 2/4 channels");
    const auto pi = std::numbers::pi_v<double>;
    for (std::size_t i = 0; i < taps_.size(); ++i) {
        const double x = static_cast<double>(i) - 384;
        const double sinc = x == 0 ? 700.0 / 48000 : std::sin(2 * pi * 350 * x / 48000) / (pi * x);
        const double window = .42 - .5 * std::cos(2 * pi * i / 768) + .08 * std::cos(4 * pi * i / 768);
        taps_[i] = sinc * window;
    }
    const auto sum = std::accumulate(taps_.begin(), taps_.end(), 0.0);
    for (auto& tap : taps_) tap /= sum;
}
void StereoResampler::reset() {
    history_ = {}; pending_ = {}; head_ = pendingSize_ = phase_ = 0; ++metrics_.resets;
}
std::vector<StereoSample> StereoResampler::feed(std::span<const std::uint8_t> pcm) {
    // Bound allocation and CPU per call. Streaming callers split larger sources.
    if (pcm.size() > 1024 * 1024) throw ProtocolError("DSP input chunk exceeds 1 MiB");
    std::vector<StereoSample> result;
    const std::size_t width = channels_ * 2;
    result.reserve((pcm.size() / width + 48) / 48);
    for (const auto byte : pcm) {
        pending_[pendingSize_++] = byte;
        if (pendingSize_ != width) continue;
        StereoSample sample{};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto offset = width - 4 + 2 * channel;
            const int raw = pending_[offset] | (pending_[offset + 1] << 8);
            const int signedValue = raw >= 32768 ? raw - 65536 : raw;
            sample[channel] = signedValue / 32768.0;
        }
        pendingSize_ = 0;
        history_[head_] = sample;
        head_ = (head_ + 1) % history_.size(); // oldest sample, matching Python deque order
        ++metrics_.inputFrames;
        if (++phase_ != 48) continue;
        phase_ = 0;
        StereoSample output{};
        for (std::size_t i = 0; i < taps_.size(); ++i)
            for (std::size_t c = 0; c < 2; ++c)
                output[c] += history_[(head_ + i) % history_.size()][c] * taps_[i];
        if (applyStrength_) output = strength_.apply(output, metrics_);
        result.push_back(output); ++metrics_.outputFrames;
    }
    return result;
}
void StereoResampler::finish() const {
    if (pendingSize_) throw ProtocolError("PCM ends in a partial interleaved frame");
}
PacketScheduler::PacketScheduler(unsigned maxLatencyMs)
    : capacity_(maxLatencyMs), latency_(static_cast<Time::rep>(maxLatencyMs) * 1000) {
    if (maxLatencyMs < 8 || maxLatencyMs > 100) throw ProtocolError("latency must be 8..100 ms");
}
void PacketScheduler::checkTime(Time now) {
    if (now < last_ || now.count() < 0 || now > Time::max() - Time(8000))
        throw ProtocolError("invalid scheduler monotonic time");
    last_ = now;
}
void PacketScheduler::reset(Time now) {
    checkTime(now); dropped_ += queue_.size(); queue_.clear(); next_ = now;
}
void PacketScheduler::enqueue(std::span<const StereoSample> samples, Time now) {
    // Validate before mutating queue, including data that would be dropped.
    for (auto s : samples)
        if (!std::isfinite(s[0]) || !std::isfinite(s[1])) throw ProtocolError("nonfinite queued sample");
    checkTime(now);
    const auto skip = samples.size() > capacity_ ? samples.size() - capacity_ : 0;
    dropped_ += skip;
    for (const auto& sample : samples.subspan(skip)) {
        if (queue_.size() == capacity_) { queue_.pop_front(); ++dropped_; }
        queue_.push_back({sample, now});
    }
}
std::optional<Frame> PacketScheduler::tick(Time now) {
    checkTime(now);
    if (now < next_) return std::nullopt;
    const auto missed = (now - next_).count() / 8000;
    // Queue is at most 100 items; avoid overflow for very large time advances.
    auto discard = missed >= 13 ? queue_.size() : std::min(queue_.size(), static_cast<std::size_t>(missed) * 8);
    while (discard--) { queue_.pop_front(); ++dropped_; }
    while (!queue_.empty() && now - queue_.front().arrived > latency_) { queue_.pop_front(); ++dropped_; }
    std::array<StereoSample, 8> block{};
    const auto count = std::min<std::size_t>(8, queue_.size());
    for (std::size_t i = 0; i < count; ++i) { block[i] = queue_.front().sample; queue_.pop_front(); }
    underruns_ += 8 - count;
    next_ = now + Time(8000 - (now - next_).count() % 8000);
    return gripWaveform(block);
}
}
