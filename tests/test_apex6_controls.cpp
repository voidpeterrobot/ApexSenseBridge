#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "platform/GripControl.h"
#include "platform/Apex6Settings.h"
#include "core/ControllerCapabilities.h"
#include "flydigi/Apex5Device.h"
#include "platform/Apex6IsolationPolicy.h"
#include "apex6/RawLibraryContract.h"
#include <limits>
#include <filesystem>
#include <thread>
#include <iostream>
#include <cstring>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(...){caught=true;}check(caught,"expected rejection");}
int main(){try{
    using namespace asb::platform;
    check(detail::sameRecoveryOwner(42,100,42,100),"current recovery owner rejected");
    check(!detail::sameRecoveryOwner(43,100,42,100),"stale watchdog PID accepted");
    check(!detail::sameRecoveryOwner(42,101,42,100),"reused watchdog PID accepted");
    check(detail::sameRecoveryOwner(42,0,42,0),"legacy recovery marker rejected");
    const std::string digest(64,'a');
    check(asb::apex6::matchesRawLibraryContract("Integrated library version: v0.7.0-asb9\nArtifact SHA-256: "+digest+"\n",digest),"asb9 provenance rejected");
    check(!asb::apex6::matchesRawLibraryContract("Integrated library version: v0.7.0-asb8\nArtifact SHA-256: "+digest+"\n",digest),"asb8 accepted despite shared capability bits");
    check(!asb::apex6::matchesRawLibraryContract("Integrated library version: v0.7.0-asb9\nArtifact SHA-256: "+digest+"\n",std::string(64,'b')),"DLL hash mismatch accepted");
    detail::IsolationState before{false,false,{L"prior.exe"},{}},owned{true,false,{L"engine.exe"},{L"physical",L"vendor",L"proxy"}};
    check(detail::matchesApex6Isolation(owned,owned),"strict isolation baseline");
    auto concurrent=owned;concurrent.apps.push_back(L"new.exe");concurrent.devices.push_back(L"unrelated");
    check(!detail::matchesApex6Isolation(concurrent,owned),"allowlist/target change ignored");
    const auto restored=detail::restoreApex6Isolation(concurrent,before,owned);
    check(restored.active&&restored.devices==std::vector<std::wstring>{L"unrelated"}&&detail::isolationContains(restored.apps,L"prior.exe")&&detail::isolationContains(restored.apps,L"new.exe")&&!detail::isolationContains(restored.apps,L"engine.exe"),"restoration overwrote concurrent configuration");
    check(!detail::restoreApex6Isolation(owned,before,owned).active,"restoration left session cloaked");
    concurrent=owned;concurrent.inverse=true;check(!detail::matchesApex6Isolation(concurrent,owned),"inverse mode ignored");
    wchar_t prior[32768];auto count=GetEnvironmentVariableW(L"LOCALAPPDATA",prior,32768);
    auto root=std::filesystem::temp_directory_path()/("asb-apex6-controls-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    struct Restore {std::wstring value;~Restore(){SetEnvironmentVariableW(L"LOCALAPPDATA",value.c_str());}} restore{std::wstring(prior,count)};
    SetEnvironmentVariableW(L"LOCALAPPDATA",root.c_str());
    auto defaults=readApex6Settings();check(defaults.consentVersion==0&&defaults.gain==1,"consent migration defaults");
    updateApex6Settings(1,std::nullopt);updateApex6Settings(std::nullopt,4.5);
    std::thread first([]{for(unsigned i=0;i<20;++i)updateApex6Settings(1,std::nullopt);});
    std::thread second([]{for(unsigned i=0;i<20;++i)updateApex6Settings(std::nullopt,2.5);});first.join();second.join();
    auto settings=readApex6Settings();check(settings.consentVersion==1&&settings.gain==2.5,"concurrent settings lost updates");
    rejects([]{updateApex6Settings(std::nullopt,13);});
    const std::string token="0123456789abcdef0123456789abcdef";
    auto control=createGripControl(token,1);check(!control->poll(),"spurious initial command");
    rejects([&]{createGripControl(token,1);});
    const auto name=L"Local\\ApexSenseBridge.Grip.v1."+std::wstring(token.begin(),token.end());
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());check(mapping!=nullptr,"control mapping missing");
    auto bytes=static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,56));check(bytes!=nullptr,"control mapping view");
    const double gain=6.5;const std::uint32_t revision=1;
    std::memcpy(bytes+48,&gain,8);std::memcpy(bytes+40,&revision,4);
    check(control->poll()==gain&&!control->poll(),"gain command/revision handling");
    bytes[8]='f';rejects([&]{control->poll();});bytes[8]='0';
    bytes[4]=2;rejects([&]{control->poll();});bytes[4]=1;
    const double invalid=std::numeric_limits<double>::quiet_NaN();std::memcpy(bytes+48,&invalid,8);rejects([&]{control->poll();});
    UnmapViewOfFile(bytes);CloseHandle(mapping);
    asb::HidDeviceInfo d;d.vendorId=0x37d7;d.productId=0x2502;d.usagePage=0xffa0;d.interfaceNumber=L"MI_02";d.inputReportLength=d.outputReportLength=33;
    d.containerId=L"dynamic-container";d.instanceId=L"HID\\VID_37D7&PID_2502&MI_02\\dynamic";d.parentInstanceId=L"USB\\VID_37D7&PID_2502&MI_02\\dynamic";
    check(asb::isApex6Vendor(d)&&!asb::controllerCapabilities(d).adaptiveTriggers,"Apex6 capabilities");
    auto other=d;other.parentInstanceId=L"ROOT\\unrelated";check(!asb::isApex6Vendor(other),"unverified ancestry selected");
    other=d;other.productId=0x2401;check(!asb::isApex6Vendor(other),"wireless selected");
    std::string error;check(!asb::flydigi::Apex5Device::open(d,error)&&error.find("unsupported")!=std::string::npos,"Apex6 routed into trigger/profile transport");
    std::cout<<"Apex6 settings, IPC, controller routing tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
