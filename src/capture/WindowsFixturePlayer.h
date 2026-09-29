#pragma once
#include "capture/FeedbackRecorder.h"
#include <atomic>
#include <thread>
namespace asb::capture {
class WindowsFixturePlayer {
public:
    WindowsFixturePlayer()=default;
    ~WindowsFixturePlayer();
    void start(std::filesystem::path wav,std::string virtualSerial,FeedbackRecorder& recorder);
    void stop(bool orderly = false);
    bool done() const noexcept { return done_.load(); }
    const std::string& endpoint() const { return endpoint_; } // read after stop/join
    const std::string& error() const { return error_; } // read after stop/join
private:
    void run(const std::filesystem::path& wav,const std::string& serial,FeedbackRecorder& recorder) noexcept;
    std::thread worker_;
    std::atomic_bool stop_{false},done_{false},orderly_{false};
    std::string endpoint_;
    std::string error_;
};
}
