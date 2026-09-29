#include "capture/FeedbackRecorder.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace asb::capture {
FeedbackRecorder::FeedbackRecorder(std::filesystem::path directory, Limits limits,
                                   std::function<void()> beforeWrite)
    : directory_(std::move(directory)), limits_(limits), beforeWrite_(std::move(beforeWrite)) {}
void FeedbackRecorder::metadata(std::string key,std::string value) {
    if(key.size()>128 || value.size()>1024 || metadata_.size()>=32)
        throw std::runtime_error("session metadata limit exceeded");
    std::size_t total=key.size()+value.size();
    for(const auto& [k,v]:metadata_) if(k!=key) total+=k.size()+v.size();
    if(total>4096) throw std::runtime_error("session metadata byte budget exceeded");
    metadata_[std::move(key)]=std::move(value);
}
FeedbackRecorder::~FeedbackRecorder() {
    if (writer_.joinable()) {
        fail("recorder abandoned without finalization");
        { std::lock_guard lock(mutex_); stopping_ = true; }
        condition_.notify_one(); writer_.join();
    }
}
void FeedbackRecorder::fail(const char* reason) noexcept {
    const char* empty = nullptr; failure_.compare_exchange_strong(empty, reason);
}
bool FeedbackRecorder::start(std::string& error) {
    error.clear();
    try {
        if (started_) throw std::runtime_error("recorder already started");
        if ((!limits_.continuous && !limits_.seconds) || limits_.seconds > 3600 || limits_.metadataReserve < 65536 ||
            limits_.outputBytes <= limits_.metadataReserve || limits_.outputBytes > 1024ULL*1024*1024 ||
            !limits_.queueBytes || limits_.queueBytes > limits_.outputBytes)
            throw std::runtime_error("invalid recorder limits");
        // create_directory is the no-overwrite claim; never accept an existing directory.
        if (!std::filesystem::create_directory(directory_)) throw std::runtime_error("output directory already exists");
        transfers_.exceptions(std::ios::badbit | std::ios::failbit);
        events_.exceptions(std::ios::badbit | std::ios::failbit);
        transfers_.open(directory_/"transfers.bin", std::ios::binary);
        events_.open(directory_/"events.jsonl", std::ios::binary);
        writeSession(false, "initializing", "");
        started_ = true;
        writer_ = std::thread(&FeedbackRecorder::writeLoop, this);
        return true;
    } catch (const std::exception& e) { fail("recorder startup failed"); error=e.what(); return false; }
}
void FeedbackRecorder::submit(std::span<const std::uint8_t> bytes,bool terminalDisconnectExpected) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        if (failed()) { ++rejectedRecords_; return; }
        if (!started_ || bytes.size() < headerSize || bytes.size() > maxRecordSize) {
            ++rejectedRecords_; fail("callback record outside ABI bounds"); return;
        }
        const auto charge = bytes.size() + sizeof(Pending) + 128;
        // Reserve an upper bound for each fixed-size JSON index entry too.
        const auto storageCharge = bytes.size() + 1024;
        if (charge > limits_.queueBytes - pendingBytes_) { ++rejectedRecords_; fail("capture queue exhausted"); return; }
        if (!limits_.continuous && storageCharge > limits_.outputBytes-limits_.metadataReserve-reservedBytes_) {
            ++rejectedRecords_; fail("capture storage budget exhausted"); return;
        }
        queue_.push_back({std::vector<std::uint8_t>(bytes.begin(), bytes.end()), std::chrono::steady_clock::now(),terminalDisconnectExpected});
        pendingBytes_ += charge; if(!limits_.continuous) reservedBytes_ += storageCharge;
        peakBytes_ = (std::max)(peakBytes_,pendingBytes_);
        condition_.notify_one();
    } catch (...) { ++rejectedRecords_; fail("callback copy allocation failed"); }
}
void FeedbackRecorder::writeLoop() noexcept {
    try {
        for (;;) {
            Pending pending;
            { std::unique_lock lock(mutex_);
              condition_.wait(lock,[&]{return stopping_ || !queue_.empty();});
              if (queue_.empty()) return;
              pending = std::move(queue_.front()); queue_.pop_front(); }
            if(beforeWrite_) beforeWrite_();
            writeRecord(pending);
            { std::lock_guard lock(mutex_); pendingBytes_ -= pending.data.size()+sizeof(Pending)+128; }
        }
    } catch (...) { fail("capture writer failed"); }
}
void FeedbackRecorder::writeRecord(const Pending& pending) {
    const auto& bytes = pending.data;
    transfers_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    RecordView record; std::string error;
    const bool decoded = decode(bytes,record,error);
    const auto residence = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-pending.queued).count();
    maxResidenceUs_ = (std::max)(maxResidenceUs_,static_cast<std::uint64_t>(residence));
    events_ << "{\"offset\":" << offset_ << ",\"length\":" << bytes.size()
            << ",\"decoded\":" << (decoded?"true":"false") << ",\"sequence\":" << record.sequence
            << ",\"generation\":" << record.generation << ",\"timestamp_ns\":" << record.timestampNs
            << ",\"kind\":" << static_cast<unsigned>(record.kind)
            << ",\"valid_pcm\":" << (record.validPcm?"true":"false")
            << ",\"queue_residence_us\":" << residence << "}\n";
    offset_ += bytes.size(); ++records_;
    if (!decoded) { ++invalid_; fail("unsupported or malformed capture record (raw retained)"); return; }
    if (record.sequence != lastSequence_+1 || record.generation < lastGeneration_ || record.timestampNs < lastTimestamp_)
        fail("capture sequence/generation/time discontinuity");
    if(lastSequence_ && record.timestampNs>=lastTimestamp_) maxIntervalNs_=(std::max)(maxIntervalNs_,record.timestampNs-lastTimestamp_);
    lastSequence_=record.sequence; lastGeneration_=record.generation; lastTimestamp_=record.timestampNs;
    packets_+=record.packets.size();
    for(const auto& packet:record.packets) failedPackets_+=packet.status!=0;
    if (record.kind == Kind::Event) {
        const auto event = u32(record.payload,0);
        if (event == 6) fail("backend capture failure or unsupported format");
        // Classify at callback receipt, not when the writer eventually drains.
        // Otherwise normal duration-end teardown can race with finalization, or
        // an earlier unexpected disconnect can be hidden by a later finish().
        if (event == 2 && !pending.terminalDisconnectExpected)
            fail("virtual host disconnected unexpectedly");
        return;
    }
    if (record.kind == Kind::Hid) { ++hidRecords_; return; }
    ++audioRecords_;
    if (!record.validPcm) { ++invalid_; fail("invalid PCM packet evidence (raw retained)"); return; }
    for (const auto& packet : record.packets) {
        for (std::size_t o=packet.offset; o<static_cast<std::size_t>(packet.offset)+packet.requested; o+=8) {
            ++frames_;
            for (std::size_t channel=0; channel<4; ++channel) {
                auto& s = samples_[channel];
                const auto value = std::bit_cast<std::int16_t>(u16(record.payload,o+channel*2));
                s.minimum=(std::min)(s.minimum,static_cast<std::int32_t>(value));
                s.maximum=(std::max)(s.maximum,static_cast<std::int32_t>(value));
                ++s.count; s.nonzero += value!=0; s.fullScale += value==-32768 || value==32767;
                s.sum += value; s.sumSquares += static_cast<std::uint64_t>(static_cast<std::int64_t>(value)*value);
            }
        }
    }
}
void FeedbackRecorder::writeSession(bool complete, const std::string& source, const std::string& hash) {
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(directory_/"session.json", std::ios::binary|std::ios::trunc);
    out << "{\"schema\":1,\"capture_abi\":1,\"complete\":" << (complete?"true":"false")
        << ",\"source\":" << jsonString(source) << ",\"backend_sha256\":" << jsonString(hash)
        << ",\"validation\":\"unvalidated-host-capture\",\"physical_output\":\"unavailable\""
        << ",\"clock\":\"backend-session-monotonic-nanoseconds\",\"clock_frequency\":1000000000"
        << ",\"format\":{\"rate\":48000,\"channels\":4,\"encoding\":\"s16le\"}"
        << ",\"status_semantics\":\"USB/IP OUT submission; requested packet lengths used for analysis\""
        << ",\"limits\":{\"continuous\":" << (limits_.continuous?"true":"false") << ",\"seconds\":" << limits_.seconds << ",\"output_bytes\":" << (limits_.continuous?0:limits_.outputBytes)
        << ",\"queue_bytes\":" << limits_.queueBytes << ",\"metadata_reserve\":" << limits_.metadataReserve << '}'
        << ",\"failure\":" << jsonString(failure()?failure():"")
        << ",\"records\":" << records_ << ",\"audio_records\":" << audioRecords_ << ",\"hid_records\":" << hidRecords_
        << ",\"invalid_records\":" << invalid_ << ",\"pcm_frames\":" << frames_
        << ",\"packet_count\":" << packets_ << ",\"failed_packets\":" << failedPackets_
        << ",\"rejected_records\":" << rejectedRecords_.load() << ",\"raw_bytes\":" << offset_
        << ",\"max_inter_record_ns\":" << maxIntervalNs_
        << ",\"queue_peak_bytes\":" << peakBytes_ << ",\"queue_max_residence_us\":" << maxResidenceUs_
        << ",\"channels\":[";
    for (std::size_t i=0;i<4;++i) {
        const auto& s=samples_[i]; if(i) out << ',';
        out << "{\"index\":" << i << ",\"samples\":" << s.count << ",\"min\":" << (s.count?s.minimum:0)
            << ",\"max\":" << (s.count?s.maximum:0) << ",\"nonzero\":" << s.nonzero
            << ",\"full_scale\":" << s.fullScale << ",\"mean\":" << (s.count?static_cast<double>(s.sum)/s.count:0)
            << ",\"rms\":" << (s.count?std::sqrt(static_cast<double>(s.sumSquares)/s.count):0) << '}';
    }
    out << "],\"metadata\":{";
    bool first=true;
    for(const auto& [key,value]:metadata_) {
        if(!first) out << ','; first=false; out << jsonString(key) << ':' << jsonString(value);
    }
    out << "}}\n"; out.close();
}
bool FeedbackRecorder::finish(const std::string& source, const std::string& hash, std::string& error) {
    error.clear();
    if (!started_ || finalized_) { error="recorder not active"; return false; }
    { std::lock_guard lock(mutex_); stopping_=true; }
    condition_.notify_one(); if(writer_.joinable()) writer_.join();
    finalized_=true;
    try {
        transfers_.close(); events_.close();
        if (!records_) fail("no raw feedback records received");
        writeSession(!failed(),source,hash);
        std::ofstream manifest; manifest.exceptions(std::ios::badbit|std::ios::failbit);
        manifest.open(directory_/"manifest.json",std::ios::binary);
        manifest << "{\"schema\":1,\"complete\":" << (failed()?"false":"true") << ",\"sha256\":{";
        bool first=true;
        for (const auto* file : {"session.json","transfers.bin","events.jsonl"}) {
            if(!first) manifest << ','; first=false;
            manifest << jsonString(file) << ':' << jsonString(sha256File(directory_/file));
        }
        manifest << "}}\n"; manifest.close();
    } catch (const std::exception& e) { fail("capture finalization failed"); error=e.what(); return false; }
    if(failed()) { error=failure(); return false; }
    return true;
}
} // namespace asb::capture
