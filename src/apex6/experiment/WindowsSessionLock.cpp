#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include "apex6/experiment/WindowsIo.h"

namespace asb::apex6::experiment {
std::wstring experimentLockName(const std::string& container) {
    GUID id{};const auto value=wide(container);
    if(value.empty()||FAILED(CLSIDFromString(value.c_str(),&id))||id==GUID{})throw ProtocolError("lock requires nonzero container GUID");
    wchar_t canonical[40]{};if(!StringFromGUID2(id,canonical,40))throw ProtocolError("container normalization failed");
    return std::wstring(L"Global\\ApexSenseBridge.Apex6.Experiment.")+canonical;
}
ExperimentLock::ExperimentLock(const std::string& container) {
    const auto name=experimentLockName(container);
    auto h=CreateMutexW(nullptr,FALSE,name.c_str());
    if(!h)throw ProtocolError("cooperative lock inaccessible (Win32 "+std::to_string(GetLastError())+")");
    auto status=WaitForSingleObject(h,0);
    if(status!=WAIT_OBJECT_0) {
        if(status==WAIT_ABANDONED)ReleaseMutex(h);CloseHandle(h);
        throw ProtocolError(status==WAIT_ABANDONED?"cooperative lock abandoned; investigate previous run":status==WAIT_TIMEOUT?"another experiment owns cooperative lock":"cooperative lock wait failed");
    }
    handle_=h;
}
ExperimentLock::~ExperimentLock(){if(handle_){ReleaseMutex(handle_);CloseHandle(handle_);}}
}
