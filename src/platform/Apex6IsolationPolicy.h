#pragma once
#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <string>
#include <vector>

namespace asb::platform::detail {
inline bool sameRecoveryOwner(std::uint32_t currentPid,std::uint64_t currentBirth,
                              std::uint32_t expectedPid,std::uint64_t expectedBirth) {
    return currentPid==expectedPid && (!expectedBirth || currentBirth==expectedBirth);
}
struct IsolationState {
    bool active=false,inverse=false;
    std::vector<std::wstring> apps,devices;
};
inline bool isolationContains(const std::vector<std::wstring>& list,const std::wstring& value) {
    return std::any_of(list.begin(),list.end(),[&](const auto& item){return item.size()==value.size()&&std::equal(item.begin(),item.end(),value.begin(),[](wchar_t a,wchar_t b){return std::towupper(a)==std::towupper(b);});});
}
inline bool sameIsolationList(const std::vector<std::wstring>& a,const std::vector<std::wstring>& b) {
    return a.size()==b.size()&&std::all_of(a.begin(),a.end(),[&](const auto& p){return isolationContains(b,p);});
}
inline bool matchesApex6Isolation(const IsolationState& current,const IsolationState& expected) {
    return current.active&&!current.inverse&&sameIsolationList(current.apps,expected.apps)&&sameIsolationList(current.devices,expected.devices);
}
// Mirrors Restore-Apex6HidHidePlan: remove session additions and put back the
// removed allowlist entries; leave unrelated concurrent changes untouched.
inline IsolationState restoreApex6Isolation(IsolationState current,const IsolationState& before,const IsolationState& owned) {
    std::erase_if(current.devices,[&](const auto& p){return isolationContains(owned.devices,p)&&!isolationContains(before.devices,p);});
    std::erase_if(current.apps,[&](const auto& p){return isolationContains(owned.apps,p)&&!isolationContains(before.apps,p);});
    for(const auto& p:before.apps)if(!isolationContains(current.apps,p))current.apps.push_back(p);
    if(current.devices.empty()&&!current.inverse)current.active=before.active;
    return current;
}
}
