#pragma once
#include "capture/RawFeedbackSink.h"
#include "dualsense/DualSenseInput.h"
#include <filesystem>
#include <string>
#include <memory>

namespace asb::capture {
class WindowsCaptureBackend {
public:
    explicit WindowsCaptureBackend(RawFeedbackSink& recorder);
    ~WindowsCaptureBackend();
    bool open(const std::filesystem::path& library, std::string& error);
    bool update(const dualsense::DualSenseInputState& input, std::string& error);
    bool close(std::string& error) noexcept;
    bool disconnected() const noexcept;
    std::string serial() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace asb::capture
