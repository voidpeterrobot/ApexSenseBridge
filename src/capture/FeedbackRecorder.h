#pragma once
#include "capture/RawCapture.h"
#include "capture/RawFeedbackSink.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <map>
#include <thread>

namespace asb::capture {
struct Limits {
    std::size_t outputBytes = 64*1024*1024;
    std::size_t queueBytes = 4*1024*1024;
    std::size_t metadataReserve = 64*1024;
    unsigned seconds = 60;
    // Explicit live-session mode: bounded RAM queue, disk-backed evidence until
    // operator stop or I/O failure. Default capture budgets remain unchanged.
    bool continuous = false;
};
struct SampleMetrics {
    std::int32_t minimum = 32767, maximum = -32768;
    std::uint64_t count = 0, nonzero = 0, fullScale = 0, sumSquares = 0;
    std::int64_t sum = 0;
};
// No virtual or physical transport dependency: owned, bounded raw evidence only.
class FeedbackRecorder : public RawFeedbackSink {
public:
    // Optional writer-thread hook permits deterministic slow-I/O/failure tests;
    // production callers leave it empty. It is never run on the callback thread.
    FeedbackRecorder(std::filesystem::path directory, Limits limits = {},
                     std::function<void()> beforeWrite = {});
    ~FeedbackRecorder();
    FeedbackRecorder(const FeedbackRecorder&) = delete;
    FeedbackRecorder& operator=(const FeedbackRecorder&) = delete;
    bool start(std::string& error);
    // Main/session owner only; bounded metadata, serialized after callbacks stop.
    void metadata(std::string key, std::string value);
    void submit(std::span<const std::uint8_t> record, bool terminalDisconnectExpected = false) noexcept;
    void fail(const char* staticReason) noexcept;
    bool finish(const std::string& source, const std::string& backendHash, std::string& error);
    bool failed() const noexcept { return failure_.load() != nullptr; }
    const char* failure() const noexcept { return failure_.load(); }
private:
    struct Pending {
        std::vector<std::uint8_t> data;
        std::chrono::steady_clock::time_point queued;
        bool terminalDisconnectExpected = false;
    };
    void writeLoop() noexcept;
    void writeRecord(const Pending& pending);
    void writeSession(bool complete, const std::string& source, const std::string& backendHash);
    std::filesystem::path directory_;
    Limits limits_;
    std::ofstream transfers_, events_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Pending> queue_;
    std::thread writer_;
    std::atomic<const char*> failure_{nullptr};
    bool stopping_ = false, started_ = false, finalized_ = false;
    std::size_t pendingBytes_ = 0, peakBytes_ = 0, reservedBytes_ = 0;
    std::uint64_t offset_ = 0, records_ = 0, audioRecords_ = 0, hidRecords_ = 0;
    std::uint64_t invalid_ = 0, frames_ = 0, lastSequence_ = 0, lastGeneration_ = 0, lastTimestamp_ = 0;
    std::uint64_t packets_ = 0, failedPackets_ = 0, maxIntervalNs_ = 0;
    std::atomic_uint64_t rejectedRecords_{0};
    std::uint64_t maxResidenceUs_ = 0;
    std::array<SampleMetrics,4> samples_{};
    std::map<std::string,std::string> metadata_;
    std::function<void()> beforeWrite_;
};
std::string sha256File(const std::filesystem::path& path);
} // namespace asb::capture
