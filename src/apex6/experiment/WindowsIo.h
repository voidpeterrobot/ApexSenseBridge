#pragma once
#include "apex6/experiment/Session.h"
#include "apex6/experiment/Pulse.h"
#include "core/DeviceInfo.h"
#include <memory>
#include <filesystem>
#ifdef ASB_APEX6_LIVE_RUNNER
#include "apex6/live/Live.h"
#endif

namespace asb::apex6::experiment {
std::string utf8(const std::wstring&);
std::wstring wide(const std::string&);
// Metadata discovery uses access=0; selection must be an exact instance ID.
std::vector<HidDeviceInfo> vendorInterfaces();
Binding inspectInterface(const HidDeviceInfo&);
std::uint32_t queryShareFlags(AccessMode);
std::wstring experimentLockName(const std::string& container);
class ExperimentLock {
public:
    explicit ExperimentLock(const std::string& container);
    ~ExperimentLock();
    ExperimentLock(const ExperimentLock&)=delete;
    ExperimentLock& operator=(const ExperimentLock&)=delete;
private:
    void* handle_=nullptr;
};
struct AccessProbeResult {
    std::string name;
    std::uint32_t access=0,share=0,openError=0,closeError=0;
    bool opened=false,closed=false;
};
// Diagnostic only: seven fixed open/close cases, no report operations. Must
// run in a supervised worker. Never supplies a handle to the query transport.
std::vector<AccessProbeResult> probeInterfaceAccess(const Binding&);
// Query-only factory. The resulting transport MUST live in a supervised worker.
struct NativeTiming {
    Time observed{}; const char* event=""; std::uint64_t operation=0;
    std::uint32_t transferred=0,error=0;
    std::array<std::uint8_t,65> raw{};std::size_t rawSize=0;
    bool waitMetadata=false;
    Time deadline{};
    std::uint32_t requestedWaitMs=0,waitResult=0;
};
class WindowsTransport : public Io {
public:
    virtual std::span<const NativeTiming> timings() const = 0;
    virtual bool timingComplete() const = 0;
    // Stops submissions and resolves/cancels a pending read within a bounded wait.
    // False retains kernel-owned memory/handle until supervised process teardown.
    virtual bool finish() = 0;
    virtual bool inputOnly() const = 0;
    virtual bool ram5Only() const = 0;
#ifdef ASB_APEX6_LIVE_RUNNER
    virtual void stopLive(Time) = 0;
    virtual void drainLiveTimings(const std::function<void(const NativeTiming&)>&) = 0;
#endif
};
std::unique_ptr<WindowsTransport> openQueryTransport(const Binding&);
// Shared, fixed 14-query sequence; rejects every additional/out-of-order write.
std::unique_ptr<WindowsTransport> openGripBaselineTransport(const Binding&);
#ifdef ASB_APEX6_LIVE_RUNNER
std::unique_ptr<WindowsTransport> openLiveTransport(const live::Authorization&,const std::function<std::int64_t()>&,const std::function<bool()>&);
#ifdef ASB_APEX6_INTEGRATED
// Production entry points have no experimental approval-file decoder.
std::unique_ptr<WindowsTransport> openIntegratedBaselineTransport(const Binding&,const std::function<bool()>& cancelled);
std::unique_ptr<WindowsTransport> openIntegratedTransport(const GripBaseline&,live::Policy,
    const std::function<bool()>& cancelled,const std::function<void()>& entryCheck);
#endif
#endif
// Fixed shared GENERIC_READ only. Native write() rejects every report.
std::unique_ptr<WindowsTransport> openInputListener(const Binding&);
struct ListenResult { unsigned reports=0; Time elapsed{}; bool complete=false; std::string stop,error; };
// Fixed three-second / 64-report ceiling. Raw reports are unsolicited, not replies.
ListenResult listenInput(WindowsTransport&,Trace&,const std::function<void()>& idle);
// One exact A3 [01 05] write maximum; no general query capability or retry.
std::unique_ptr<WindowsTransport> openRam5Diagnostic(const Binding&);
struct Ram5Result {
    unsigned writes=0,before=0,after=0,candidates=0,unexpected=0;
    bool complete=false;std::string stop,error;
};
// 250 ms pre-listen, 600 ms from write intent, 64 total input reports.
// Unexpected reports are diagnostic evidence, never successful acquisitions.
Ram5Result examineRam5(WindowsTransport&,Trace&,const std::function<void()>& idle);
#ifdef ASB_APEX6_NEUTRAL_RUNNER
std::unique_ptr<WindowsTransport> openNeutralTransport(const NeutralAuthorization&,const std::function<std::int64_t()>& clock);
std::unique_ptr<WindowsTransport> openGripNeutralTransport(const GripNeutralAuthorization&,const std::function<std::int64_t()>& clock);
std::unique_ptr<WindowsTransport> openGripRestoreTransport(const GripRestoreAuthorization&,const std::function<std::int64_t()>& clock);
std::unique_ptr<WindowsTransport> openGripLifecycleTransport(const GripLifecycleAuthorization&,const std::function<std::int64_t()>& clock);
std::unique_ptr<WindowsTransport> openGripPulseTransport(const GripPulseAuthorization&,const std::function<std::int64_t()>& clock,const std::function<bool()>& cancelled);
#endif
std::string sha256(std::span<const std::uint8_t>);
std::string sha256File(const std::filesystem::path&);
}
