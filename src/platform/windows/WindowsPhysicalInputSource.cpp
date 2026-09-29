#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>

#include "platform/PhysicalInputSource.h"

#include "flydigi/Apex4Input.h"
#include "flydigi/Apex4Protocol.h"
#include "platform/HidTransport.h"
#include "platform/XInputGamepad.h"
#include "platform/XInputMapping.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace asb::platform {
namespace {

std::string win32Error(DWORD code) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string message = size && buffer ? std::string(buffer, size)
                                         : "Unknown Win32 error";
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.pop_back();
    }
    return message;
}

DWORD waitMilliseconds(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) return 0;
    constexpr auto maximum =
        static_cast<long long>((std::numeric_limits<DWORD>::max)() - 1);
    return static_cast<DWORD>((std::min)(timeout.count(), maximum));
}

class Apex4PhysicalInputSource final : public PhysicalInputSource {
public:
    static std::unique_ptr<Apex4PhysicalInputSource> open(
        const HidDeviceInfo& vendorInterface, std::string& error) {
        if (!flydigi::isApex4Product(
                vendorInterface.vendorId, vendorInterface.productId) ||
            vendorInterface.usagePage != flydigi::kApex4VendorUsagePage ||
            vendorInterface.inputReportLength < 32) {
            error = "The selected interface is not a complete Apex 4 V1 vendor interface.";
            return {};
        }

        HANDLE handle = CreateFileW(
            vendorInterface.path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            error = "Could not open the selected Apex 4 V1 input stream (" +
                    std::to_string(code) + ": " + win32Error(code) + ").";
            return {};
        }

        auto source = std::unique_ptr<Apex4PhysicalInputSource>(
            new Apex4PhysicalInputSource(vendorInterface, handle));
        if (!source->event_) {
            error = "Could not create the Apex 4 V1 input event.";
            return {};
        }
        return source;
    }

    ~Apex4PhysicalInputSource() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (readPending_) {
                CancelIoEx(handle_, &overlapped_);
                DWORD ignored = 0;
                (void)GetOverlappedResult(handle_, &overlapped_, &ignored, TRUE);
            }
            CloseHandle(handle_);
        }
        if (event_) CloseHandle(event_);
    }

    PhysicalInputStatus waitForState(
        dualsense::DualSenseInputState& state,
        std::chrono::milliseconds timeout,
        std::string& error) override {
        error.clear();
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        for (;;) {
            if (!ensureReadPending(error)) return PhysicalInputStatus::Error;

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                ++stats_.timeouts;
                return PhysicalInputStatus::Timeout;
            }
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            if (remaining.count() == 0) remaining = std::chrono::milliseconds(1);

            const DWORD waitResult = WaitForSingleObject(
                event_, waitMilliseconds(remaining));
            if (waitResult == WAIT_TIMEOUT) {
                ++stats_.timeouts;
                return PhysicalInputStatus::Timeout;
            }
            if (waitResult != WAIT_OBJECT_0) {
                const auto code = GetLastError();
                error = "Waiting for an Apex 4 V1 input report failed (" +
                        std::to_string(code) + ": " + win32Error(code) + ").";
                return PhysicalInputStatus::Error;
            }

            DWORD bytesRead = 0;
            readPending_ = false;
            if (!GetOverlappedResult(handle_, &overlapped_, &bytesRead, FALSE)) {
                const auto code = GetLastError();
                if (code == ERROR_DEVICE_NOT_CONNECTED || code == ERROR_INVALID_HANDLE ||
                    code == ERROR_OPERATION_ABORTED) {
                    error = "The physical Apex 4 V1 input stream disconnected.";
                    return PhysicalInputStatus::Disconnected;
                }
                error = "Completing the Apex 4 V1 input report failed (" +
                        std::to_string(code) + ": " + win32Error(code) + ").";
                return PhysicalInputStatus::Error;
            }
            ++stats_.reports;

            const auto decoded = flydigi::decodeApex4InputReport(
                std::span<const std::uint8_t>(report_.data(), bytesRead));
            if (!decoded) {
                // Identity replies and other vendor notifications share this
                // stream. They are valid traffic, just not controller state.
                continue;
            }
            state = *decoded;
            return PhysicalInputStatus::State;
        }
    }

    std::string_view backendName() const noexcept override {
        return "apex4-v1-hid-event";
    }
    bool eventDriven() const noexcept override { return true; }
    PhysicalInputSourceStats stats() const noexcept override { return stats_; }

private:
    Apex4PhysicalInputSource(const HidDeviceInfo& info, HANDLE handle)
        : handle_(handle), report_(info.inputReportLength, 0) {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        overlapped_.hEvent = event_;
    }

    bool ensureReadPending(std::string& error) {
        if (readPending_) return true;
        ResetEvent(event_);
        std::fill(report_.begin(), report_.end(), std::uint8_t{0});
        if (ReadFile(handle_, report_.data(), static_cast<DWORD>(report_.size()),
                     nullptr, &overlapped_)) {
            SetEvent(event_);
            readPending_ = true;
            return true;
        }
        const auto code = GetLastError();
        if (code == ERROR_IO_PENDING) {
            readPending_ = true;
            return true;
        }
        error = "Starting the Apex 4 V1 input read failed (" +
                std::to_string(code) + ": " + win32Error(code) + ").";
        return false;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    OVERLAPPED overlapped_{};
    bool readPending_ = false;
    std::vector<std::uint8_t> report_;
    PhysicalInputSourceStats stats_{};
};

class XInputPhysicalInputSource final : public PhysicalInputSource {
public:
    explicit XInputPhysicalInputSource(std::unique_ptr<XInputGamepad> gamepad)
        : gamepad_(std::move(gamepad)) {
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is supported on all Windows
        // builds targeted by the installer. Fall back to a regular waitable
        // timer if a compatibility layer rejects the flag.
        constexpr DWORD kHighResolution = 0x00000002;
        timer_ = CreateWaitableTimerExW(
            nullptr, nullptr, kHighResolution, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer_) {
            timer_ = CreateWaitableTimerExW(
                nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
    }

    ~XInputPhysicalInputSource() override {
        if (timer_) CloseHandle(timer_);
    }

    PhysicalInputStatus waitForState(
        dualsense::DualSenseInputState& state,
        std::chrono::milliseconds timeout,
        std::string& error) override {
        if (timeout.count() > 0) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(timeout.count()) * 10000LL;
            if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                (void)WaitForSingleObject(timer_, INFINITE);
            } else {
                std::this_thread::sleep_for(timeout);
            }
        }
        if (!gamepad_->poll(state, error)) return PhysicalInputStatus::Disconnected;
        ++stats_.reports;
        return PhysicalInputStatus::State;
    }
    std::string_view backendName() const noexcept override { return "xinput-fallback"; }
    bool eventDriven() const noexcept override { return false; }
    PhysicalInputSourceStats stats() const noexcept override { return stats_; }

private:
    std::unique_ptr<XInputGamepad> gamepad_;
    HANDLE timer_ = nullptr;
    PhysicalInputSourceStats stats_{};
};

} // namespace

std::unique_ptr<PhysicalInputSource> openPhysicalInputSource(
    const HidDeviceInfo& apexVendorInterface,
    std::optional<unsigned int> requestedXInputIndex,
    std::string& error) {
    if (!requestedXInputIndex) {
        if (flydigi::isApex4Product(
                apexVendorInterface.vendorId, apexVendorInterface.productId)) {
            std::string apex4Error;
            auto apex4 = Apex4PhysicalInputSource::open(
                apexVendorInterface, apex4Error);
            if (apex4) {
                error.clear();
                return apex4;
            }
            error = "Apex 4 V1 input unavailable (" + apex4Error + "); ";
        }

        std::string hidError;
        auto hid = openGenericHidInputSource(apexVendorInterface, hidError);
        if (hid) {
            error.clear();
            return hid;
        }
        error += "APEX HID input unavailable (" + hidError +
                 "); trying XInput fallback.";
    }

    std::string xinputError;
    auto xinput = openXInputGamepadForDevice(
        apexVendorInterface.vendorId, apexVendorInterface.productId,
        requestedXInputIndex, xinputError);
    if (!xinput) {
        if (!error.empty()) error += ' ';
        error += "XInput fallback unavailable: " + xinputError;
        return {};
    }
    return std::make_unique<XInputPhysicalInputSource>(std::move(xinput));
}

} // namespace asb::platform

#endif // _WIN32
