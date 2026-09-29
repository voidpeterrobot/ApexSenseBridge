#pragma once
#include <cstdint>
#include <span>

namespace asb::capture {
// A callback borrows bytes only for the duration of submit. Implementations must
// copy anything retained and must not call a transport or perform disk I/O.
class RawFeedbackSink {
public:
    virtual ~RawFeedbackSink() = default;
    virtual void submit(std::span<const std::uint8_t>, bool terminalDisconnectExpected = false) noexcept = 0;
    virtual void fail(const char* staticReason) noexcept = 0;
    virtual bool failed() const noexcept = 0;
};
}
