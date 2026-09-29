#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "platform/Apex6Settings.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace asb::platform {
namespace {
struct Lock {
    HANDLE handle=CreateMutexW(nullptr,FALSE,L"Local\\ApexSenseBridge.Apex6Settings.v1");
    Lock(){if(!handle)throw std::runtime_error("Cannot open Apex6 settings lock");
        const auto wait=WaitForSingleObject(handle,5000);
        if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(handle);handle=nullptr;throw std::runtime_error("Apex6 settings are busy");}}
    ~Lock(){if(handle){ReleaseMutex(handle);CloseHandle(handle);}}
};
std::filesystem::path settingsPath(){
    wchar_t path[32768];auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",path,32768);
    if(!n||n>=32768)throw std::runtime_error("LOCALAPPDATA is unavailable");
    return std::filesystem::path(path)/L"ApexSenseBridge"/L"apex6-settings-v1.txt";
}
Apex6Settings read(const std::filesystem::path& path){
    if(!std::filesystem::exists(path))return {};
    if(std::filesystem::file_size(path)>1024)throw std::runtime_error("Invalid Apex6 settings size");
    std::ifstream in(path);std::string schema;Apex6Settings value;
    if(!(in>>schema>>value.consentVersion>>value.gain)||schema!="ASB_APEX6_SETTINGS_V1"||
        value.consentVersion>kApex6ConsentVersion||!validGripGain(value.gain))
        throw std::runtime_error("Invalid Apex6 settings; beta remains disabled");
    in>>std::ws;if(!in.eof())throw std::runtime_error("Unexpected Apex6 settings fields");
    return value;
}
}
Apex6Settings readApex6Settings(){Lock lock;return read(settingsPath());}
void updateApex6Settings(std::optional<unsigned> consent,std::optional<double> gain){
    if((consent&&*consent>kApex6ConsentVersion)||(gain&&!validGripGain(*gain)))throw std::runtime_error("Invalid Apex6 settings update");
    Lock lock;const auto path=settingsPath();auto value=read(path);
    if(consent)value.consentVersion=*consent;if(gain)value.gain=*gain;
    std::filesystem::create_directories(path.parent_path());
    auto temporary=path;temporary+=L".tmp";
    {std::ofstream out(temporary,std::ios::trunc);out<<"ASB_APEX6_SETTINGS_V1\n"<<value.consentVersion<<'\n'<<std::setprecision(17)<<value.gain<<'\n';out.flush();if(!out)throw std::runtime_error("Apex6 settings write failed");}
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Apex6 settings atomic replacement failed");
}
}
