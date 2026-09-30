#pragma once
// Narrow OS-call seam for tests. Production never supplies overrides.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "apex6/experiment/WindowsIo.h"

namespace asb::apex6::experiment {
struct WindowsIoHooks {
    std::function<BOOL(bool,HANDLE,void*,DWORD,OVERLAPPED*,DWORD&)> submit;
    std::function<DWORD(HANDLE,DWORD)> wait;
    std::function<BOOL(HANDLE,OVERLAPPED*,DWORD&,DWORD&)> result;
    std::function<void(HANDLE,OVERLAPPED*)> cancel;
    std::function<Time()> clock;
};
// Takes ownership of an inert test handle. Still enforces physical query-only
// write allowlist. No discovery/open APIs are called by this factory.
std::unique_ptr<WindowsTransport> makeQueryIoForTest(const Binding&,HANDLE,WindowsIoHooks);
std::unique_ptr<WindowsTransport> makeGripBaselineIoForTest(const Binding&,HANDLE,WindowsIoHooks);
std::unique_ptr<WindowsTransport> makeInputListenerForTest(const Binding&,HANDLE,WindowsIoHooks);
std::unique_ptr<WindowsTransport> makeRam5DiagnosticForTest(const Binding&,HANDLE,WindowsIoHooks);
#ifdef ASB_APEX6_LIVE_RUNNER
std::unique_ptr<WindowsTransport> makeLiveIoForTest(const live::Authorization&,HANDLE,WindowsIoHooks,const std::function<std::int64_t()>&,const std::function<bool()>&);
#ifdef ASB_APEX6_INTEGRATED
std::unique_ptr<WindowsTransport> makeDongleBaselineIoForTest(const Binding&,HANDLE,WindowsIoHooks);
#endif
#endif
#ifdef ASB_APEX6_NEUTRAL_RUNNER
std::unique_ptr<WindowsTransport> makeGripPulseIoForTest(const GripPulseAuthorization&,HANDLE,WindowsIoHooks,const std::function<std::int64_t()>& clock,const std::function<bool()>& cancelled);
std::unique_ptr<WindowsTransport> makeGripLifecycleIoForTest(const GripLifecycleAuthorization&,HANDLE,WindowsIoHooks,const std::function<std::int64_t()>& clock);
std::unique_ptr<Io> makeNeutralIoForTest(const NeutralAuthorization&,HANDLE,WindowsIoHooks,const std::function<std::int64_t()>& clock);
std::unique_ptr<Io> makeGripNeutralIoForTest(const GripNeutralAuthorization&,HANDLE,WindowsIoHooks,const std::function<std::int64_t()>& clock);
std::unique_ptr<WindowsTransport> makeGripRestoreIoForTest(const GripRestoreAuthorization&,HANDLE,WindowsIoHooks,const std::function<std::int64_t()>& clock);
#endif
struct WindowsAccessHooks {
    std::function<HANDLE(const std::wstring&,DWORD,DWORD,DWORD&)> open;
    std::function<BOOL(HANDLE,DWORD&)> close;
};
std::vector<AccessProbeResult> probeInterfaceAccessForTest(const Binding&,const WindowsAccessHooks&);
}
