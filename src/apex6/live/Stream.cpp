#include "apex6/live/Stream.h"
#include <algorithm>
#include <cmath>

namespace asb::apex6::live {
Time monotonic(){return std::chrono::duration_cast<Time>(std::chrono::steady_clock::now().time_since_epoch());}
void RawQueue::fail(const char* reason)noexcept {const char* empty=nullptr;failure_.compare_exchange_strong(empty,reason);}
void RawQueue::submit(std::span<const std::uint8_t> bytes,bool terminal)noexcept {
    try {
        std::lock_guard lock(mutex_);
        if(failed()){++metrics_.rejected;return;}
        if(bytes.size()<capture::headerSize||bytes.size()>capture::maxRecordSize){++metrics_.rejected;fail("raw record size outside ABI bounds");return;}
        const auto charge=bytes.size()+sizeof(RawItem)+128;
        if(charge>4*1024*1024-bytes_) {++metrics_.rejected;fail("raw queue overflow");return;}
        // Retain the offending bounded record for evidence before latching.
        queue_.push_back({Bytes(bytes.begin(),bytes.end()),clock_(),terminal});
        bytes_+=charge;metrics_.peakBytes=std::max(metrics_.peakBytes,bytes_);++metrics_.records;
        capture::RecordView r;std::string error;
        if(!capture::decode(bytes,r,error)) {++metrics_.rejected;fail("malformed raw feedback");return;}
        if(r.sequence!=sequence_+1||r.generation<generation_||r.timestampNs<timestamp_)fail("raw sequence/generation/time discontinuity");
        if(r.kind==capture::Kind::Audio&&!r.validPcm)fail("invalid raw PCM packets");
        if(r.kind==capture::Kind::Event&&(capture::u32(r.payload,0)==6||(capture::u32(r.payload,0)==2&&!terminal)))fail("raw backend failure/disconnect");
        sequence_=r.sequence;generation_=r.generation;timestamp_=r.timestampNs;
    }catch(...){fail("raw callback allocation/validation failure");}
}
std::optional<RawItem> RawQueue::pop() {
    std::lock_guard lock(mutex_);if(queue_.empty())return {};
    auto item=std::move(queue_.front());queue_.pop_front();bytes_-=item.bytes.size()+sizeof(RawItem)+128;return item;
}
QueueMetrics RawQueue::metrics()const {std::lock_guard lock(mutex_);return metrics_;}
GainRamp::GainRamp(double initial):current_(initial),start_(initial),target_(initial){GripStrength(initial,.75);}
void GainRamp::target(double value){GripStrength(value,.75);start_=current_;target_=value;remaining_=100;}
StereoSample GainRamp::apply(StereoSample value,DspMetrics& metrics) {
    if(remaining_){--remaining_;current_=start_+(target_-start_)*(100-remaining_)/100.;}
    return GripStrength(current_,.75).apply(value,metrics);
}
void Stream::discardPcm(){metrics_.droppedSamples+=samples_.size();samples_.clear();resampler_.reset();++metrics_.resets;lastAudio_.reset();}
void Stream::reset(){discardPcm();activePcm_.reset();rumbleAt_.reset();rumble_={};rumblePhase_=0;}
bool Stream::pcmOwns(Time now)const{return activePcm_&&now>=*activePcm_&&now-*activePcm_<Time(100000);}
bool Stream::ingestHid(const capture::RecordView& r,Time received,Time now){
    auto report=r.payload;
    if(r.source==3){
        // Only HID SET_REPORT(Output 0x02) on the virtual USB HID interface 3.
        if(report.size()<8||report[0]!=0x21||report[1]!=9||capture::u16(report,2)!=0x0202||capture::u16(report,4)!=3){++metrics_.hidIgnored;return false;}
        if(capture::u16(report,6)!=report.size()-8)throw ProtocolError("malformed HID output setup length");
        report=report.subspan(8);
    }
    if(report.empty()||report[0]!=2){++metrics_.hidIgnored;return false;}
    // Descriptor/common output plus known Windows USB padding variants.
    if(report.size()!=48&&report.size()!=63&&report.size()!=64)throw ProtocolError("malformed DualSense USB motor report");
    const bool compatible=(report[1]&1)||(report[39]&4);
    if(!compatible){++metrics_.hidIgnored;return false;}
    if(now<received)throw ProtocolError("reversed HID arrival clock");
    if(now-received>Time(40000)){++metrics_.staleRecords;return false;}
    // Both legacy and improved compatible rumble use the haptics-select bit.
    // A compatible update with select cleared relinquishes HID ownership.
    if(!(report[1]&2)){rumble_={};rumbleAt_.reset();++metrics_.hidIgnored;return false;}
    ++metrics_.hidAccepted;
    if(pcmOwns(now)){++metrics_.hidSuppressed;rumble_={};rumbleAt_.reset();return false;}
    rumble_={report[4]/255.0*.125,report[3]/255.0*.125};
    rumbleAt_=received;
    return report[3]||report[4];
}
bool Stream::ingest(const RawItem& item,Time now) {
    capture::RecordView r;std::string error;
    if(!capture::decode(item.bytes,r,error))throw ProtocolError("malformed stream record");
    if(r.generation<generation_||r.timestampNs<timestamp_)throw ProtocolError("old generation/time");
    if(r.generation!=generation_)reset();
    generation_=r.generation;timestamp_=r.timestampNs;
    if(r.kind==capture::Kind::Hid)return ingestHid(r,item.received,now);
    if(r.kind!=capture::Kind::Audio)return false;
    if(!r.validPcm)throw ProtocolError("invalid PCM packet");
    if(now<item.received)throw ProtocolError("reversed PCM arrival clock");
    if(now-item.received>Time(40000)){++metrics_.staleRecords;discardPcm();return false;}
    if(lastAudio_&&(item.received-*lastAudio_>Time(40000)||r.timestampNs-lastAudioTimestamp_>40000000))discardPcm();
    std::size_t frames=0;for(const auto& p:r.packets)frames+=p.requested/8;
    // A record longer than the entire latency window is stale backlog, not an
    // invitation to process seconds of DSP in one dispatch.
    if(frames>1920){++metrics_.staleRecords;discardPcm();return false;}
    const auto expected=(frames+47)/48;
    if(samples_.size()+expected>40)discardPcm();
    lastAudio_=item.received;
    lastAudioTimestamp_=r.timestampNs;
    bool active=false;
    for(const auto& p:r.packets){
        const auto pcm=r.payload.subspan(p.offset,p.requested);
        for(std::size_t i=0;i<pcm.size();i+=8)for(unsigned c:{2u,3u}){
            const auto bits=capture::u16(pcm,i+c*2);
            const int value=bits<32768?bits:int(bits)-65536;
            active|=std::abs(value)>64;
        }
        for(auto sample:resampler_.feed(pcm))samples_.push_back({sample,item.received});
    }
    if(active){activePcm_=item.received;if(rumbleAt_){++metrics_.hidSuppressed;rumbleAt_.reset();rumble_={};}}
    metrics_.peakSamples=std::max(metrics_.peakSamples,samples_.size());++metrics_.validRecords;return frames!=0;
}
Frame Stream::packet(Time now) {
    if(!samples_.empty()&&now-samples_.front().received>Time(40000))discardPcm();
    std::array<StereoSample,8> block{};
    if(rumbleAt_&&(now<*rumbleAt_||(lifetime_==HidLifetime::DiagnosticLease&&now-*rumbleAt_>=Time(2000000)))){rumbleAt_.reset();rumble_={};++metrics_.hidTimeouts;}
    const bool hid=rumbleAt_&&!pcmOwns(now)&&(rumble_[0]!=0||rumble_[1]!=0);
    for(auto& sample:block){
        if(!samples_.empty()){sample=samples_.front().value;samples_.pop_front();}else if(!hid)++metrics_.underrunSamples;
        if(hid){
            // Strength-only reports have no waveform to preserve. Synthesize
            // low/large motor on left at 80 Hz, high/small on right at 160 Hz.
            const double phase=2*3.141592653589793*(rumblePhase_++%1000)/1000.;
            sample={rumble_[0]*std::sin(80*phase),rumble_[1]*std::sin(160*phase)};
            ++metrics_.hidSamples;
        }
        sample=gain_.apply(sample,metrics_.strength);
    }
    return gripWaveform(block);
}
Stream::Source Stream::source(Time now)const {
    if(gain_.target()==0)return Source::Muted;
    if(pcmOwns(now))return Source::Pcm;
    if(rumbleAt_&&(rumble_[0]!=0||rumble_[1]!=0))return Source::Hid;
    return Source::AwaitingFeedback;
}
}
