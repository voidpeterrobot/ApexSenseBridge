#pragma once
#include "apex6/experiment/Session.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace asb::apex6::experiment {
std::string json(const std::string&);
struct TraceLimits {std::size_t total=8*1024*1024, queue=1024*1024, reserve=64*1024;};
class Evidence final:public Trace {
public:
    Evidence(const std::filesystem::path& newDirectory,TraceLimits={},std::function<void()> beforeWrite={});
    ~Evidence();
    void record(Time,const std::string&,std::span<const std::uint8_t> raw = {},std::uint64_t operation=0,std::uint64_t detail=0) override;
    // Bounded, already-serialized native timing evidence, same async writer.
    void recordLine(std::string);
    bool healthy()const override{return !failed_.load();}
    bool finish();
    std::size_t records()const{return records_;}
private:
    void loop()noexcept;
    TraceLimits limits_;std::function<void()> beforeWrite_;std::ofstream file_;
    std::mutex mutex_;std::condition_variable condition_;std::deque<std::string> queue_;
    std::thread writer_;std::atomic_bool failed_=false;
    bool stopping_=false;std::size_t bytes_=0,pending_=0,records_=0;
};
}
