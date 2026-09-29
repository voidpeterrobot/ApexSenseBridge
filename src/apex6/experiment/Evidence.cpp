#include "apex6/experiment/Evidence.h"
#include <sstream>

namespace asb::apex6::experiment {
std::string json(const std::string& value) {
    std::string out="\"";constexpr char digits[]="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}
        else if(c<32){out+="\\u00";out+=digits[c>>4];out+=digits[c&15];}
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
Evidence::Evidence(const std::filesystem::path& directory,TraceLimits limits,std::function<void()> hook):limits_(limits),beforeWrite_(std::move(hook)) {
    if(limits.total<=limits.reserve||!limits.queue)throw ProtocolError("invalid evidence limits");
    if(!std::filesystem::create_directory(directory))throw ProtocolError("evidence directory must be new");
    file_.open(directory/"trace.jsonl",std::ios::binary|std::ios::out);
    if(!file_)throw ProtocolError("cannot create evidence trace");
    writer_=std::thread([this]{loop();});
}
Evidence::~Evidence(){finish();}
void Evidence::record(Time time,const std::string& event,std::span<const std::uint8_t> raw,std::uint64_t operation,std::uint64_t detail) {
    if(raw.size()>65||event.size()>128){failed_=true;throw ProtocolError("oversized trace event");}
    std::ostringstream out;out<<"{\"time_us\":"<<time.count()<<",\"event\":"<<json(event)<<",\"operation\":"<<operation<<",\"detail\":"<<detail<<",\"raw_hex\":"<<json(hex(raw))<<"}\n";
    recordLine(out.str());
}
void Evidence::recordLine(std::string line) {
    if(line.size()>4096||line.empty()||line.back()!='\n'){failed_=true;throw ProtocolError("invalid trace line");}
    std::lock_guard lock(mutex_);
    if(failed_||stopping_||line.size()>limits_.queue-pending_||line.size()>limits_.total-limits_.reserve-bytes_) {
        failed_=true;throw ProtocolError("trace queue/storage budget exhausted or closed");
    }
    pending_+=line.size();bytes_+=line.size();++records_;queue_.push_back(std::move(line));condition_.notify_one();
}
void Evidence::loop()noexcept {
    try {
        for(;;) {
            std::string line;
            {std::unique_lock lock(mutex_);condition_.wait(lock,[&]{return stopping_||!queue_.empty();});if(queue_.empty())break;line=std::move(queue_.front());queue_.pop_front();}
            if(beforeWrite_)beforeWrite_();
            file_.write(line.data(),static_cast<std::streamsize>(line.size()));
            if(!file_)throw ProtocolError("trace storage write failed");
            {std::lock_guard lock(mutex_);pending_-=line.size();}
        }
        file_.flush();if(!file_)throw ProtocolError("trace flush failed");
    }catch(...){failed_=true;}
}
bool Evidence::finish() {
    {std::lock_guard lock(mutex_);stopping_=true;}condition_.notify_all();
    // The hardware worker's supervisor bounds a stalled filesystem operation.
    // Thread termination here is intentionally NOT advertised as a hard deadline.
    if(writer_.joinable())writer_.join();file_.close();return healthy();
}
}
