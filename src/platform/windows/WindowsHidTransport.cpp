#ifdef _WIN32

#include "platform/HidTransport.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cfgmgr32.h>
#include <combaseapi.h>
#include <devpkey.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace asb::platform {
namespace {

std::string win32Error(DWORD code) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

    std::string message = size && buffer ? std::string(buffer, size) : "Unknown Win32 error";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.pop_back();
    }
    return message;
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

DWORD waitMilliseconds(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        return 0;
    }
    constexpr auto maximum = static_cast<long long>(std::numeric_limits<DWORD>::max() - 1);
    return static_cast<DWORD>(std::min(timeout.count(), maximum));
}

class WindowsHidTransport final : public HidTransport {
public:
    WindowsHidTransport(HidDeviceInfo info, HANDLE handle)
        : info_(std::move(info)), handle_(handle) {}

    ~WindowsHidTransport() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    bool isOpen() const noexcept override {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    const HidDeviceInfo& info() const noexcept override {
        return info_;
    }

    bool writeOutputReport(std::span<const std::uint8_t> report, std::string& error) override {
        if (!isOpen()) {
            error = "HID handle is not open";
            return false;
        }
        if (info_.outputReportLength == 0) {
            error = "This HID interface declares no output report";
            return false;
        }
        if (report.size() > info_.outputReportLength) {
            std::ostringstream oss;
            oss << "Protocol report is " << report.size()
                << " bytes, but HID output report length is only " << info_.outputReportLength;
            error = oss.str();
            return false;
        }

        std::vector<std::uint8_t> wire(info_.outputReportLength, 0);
        std::copy(report.begin(), report.end(), wire.begin());

        DWORD written = 0;
        DWORD writeError = ERROR_SUCCESS;
        if (writeFileOverlapped(wire, written, writeError) && written == wire.size()) {
            return true;
        }

        if (HidD_SetOutputReport(handle_, wire.data(), static_cast<ULONG>(wire.size()))) {
            return true;
        }

        const DWORD hidError = GetLastError();
        std::ostringstream oss;
        oss << "WriteFile failed (" << writeError << ": " << win32Error(writeError)
            << "); HidD_SetOutputReport also failed (" << hidError << ": "
            << win32Error(hidError) << ")";
        error = oss.str();
        return false;
    }

    bool readFeatureReport(std::span<std::uint8_t> report, std::string& error) override {
        if (!isOpen()) {
            error = "HID handle is not open";
            return false;
        }
        if (report.empty() || report.size() > (std::numeric_limits<ULONG>::max)()) {
            error = "HID feature-report buffer has an invalid size";
            return false;
        }
        if (HidD_GetFeature(handle_, report.data(), static_cast<ULONG>(report.size()))) {
            return true;
        }

        const auto code = GetLastError();
        error = "HidD_GetFeature failed (" + std::to_string(code) + ": " +
                win32Error(code) + ')';
        return false;
    }

    HidReadStatus readInputReport(std::span<std::uint8_t> report,
                                  std::chrono::milliseconds timeout,
                                  std::size_t& bytesRead,
                                  std::string& error) override {
        bytesRead = 0;
        if (!isOpen()) {
            error = "HID handle is not open";
            return HidReadStatus::Error;
        }
        if (report.empty()) {
            error = "HID input buffer is empty";
            return HidReadStatus::Error;
        }

        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) {
            const auto code = GetLastError();
            error = "CreateEventW for HID read failed (" + std::to_string(code) +
                    ": " + win32Error(code) + ')';
            return HidReadStatus::Error;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        if (!ReadFile(handle_, report.data(), static_cast<DWORD>(report.size()),
                      nullptr, &overlapped)) {
            const auto readError = GetLastError();
            if (readError != ERROR_IO_PENDING) {
                error = "ReadFile failed (" + std::to_string(readError) +
                        ": " + win32Error(readError) + ')';
                return HidReadStatus::Error;
            }
        }

        const DWORD waitResult = WaitForSingleObject(event.get(), waitMilliseconds(timeout));
        if (waitResult == WAIT_TIMEOUT) {
            CancelIoEx(handle_, &overlapped);
            DWORD completed = 0;
            if (GetOverlappedResult(handle_, &overlapped, &completed, TRUE)) {
                bytesRead = completed;
                return completed > 0 ? HidReadStatus::Data : HidReadStatus::Timeout;
            }
            const auto completionError = GetLastError();
            if (completionError == ERROR_OPERATION_ABORTED) {
                return HidReadStatus::Timeout;
            }
            error = "HID read cancellation failed (" + std::to_string(completionError) +
                    ": " + win32Error(completionError) + ')';
            return HidReadStatus::Error;
        }
        if (waitResult != WAIT_OBJECT_0) {
            const auto waitError = GetLastError();
            CancelIoEx(handle_, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(handle_, &overlapped, &ignored, TRUE);
            error = "Waiting for HID input failed (" + std::to_string(waitError) +
                    ": " + win32Error(waitError) + ')';
            return HidReadStatus::Error;
        }

        DWORD completed = 0;
        if (!GetOverlappedResult(handle_, &overlapped, &completed, FALSE)) {
            const auto completionError = GetLastError();
            error = "Completing HID read failed (" + std::to_string(completionError) +
                    ": " + win32Error(completionError) + ')';
            return HidReadStatus::Error;
        }
        bytesRead = completed;
        return completed > 0 ? HidReadStatus::Data : HidReadStatus::Timeout;
    }

private:
    bool writeFileOverlapped(std::span<const std::uint8_t> wire,
                             DWORD& written,
                             DWORD& errorCode) {
        written = 0;
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) {
            errorCode = GetLastError();
            return false;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        if (!WriteFile(handle_, wire.data(), static_cast<DWORD>(wire.size()),
                       nullptr, &overlapped)) {
            errorCode = GetLastError();
            if (errorCode != ERROR_IO_PENDING) {
                return false;
            }
        }

        constexpr auto writeTimeout = std::chrono::milliseconds(1000);
        const DWORD waitResult = WaitForSingleObject(event.get(), waitMilliseconds(writeTimeout));
        if (waitResult != WAIT_OBJECT_0) {
            errorCode = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CancelIoEx(handle_, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(handle_, &overlapped, &ignored, TRUE);
            return false;
        }
        if (!GetOverlappedResult(handle_, &overlapped, &written, FALSE)) {
            errorCode = GetLastError();
            return false;
        }
        errorCode = written == wire.size() ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
        return written == wire.size();
    }

    HidDeviceInfo info_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

} // namespace

HidTransport* createHidTransport(const HidDeviceInfo& info, std::string& error) {
    HANDLE handle = CreateFileW(info.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        std::ostringstream oss;
        oss << "Could not open HID interface (" << code << ": "
            << win32Error(code) << ")";
        error = oss.str();
        return nullptr;
    }

    return new WindowsHidTransport(info, handle);
}

void destroyHidTransport(HidTransport* transport) noexcept {
    delete transport;
}

} // namespace asb::platform

#endif // _WIN32
