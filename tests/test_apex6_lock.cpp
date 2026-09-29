#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include "apex6/experiment/WindowsIo.h"
#include <iostream>
using namespace asb::apex6::experiment;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string container() {GUID id{};check(SUCCEEDED(CoCreateGuid(&id)),"GUID");wchar_t value[40]{};StringFromGUID2(id,value,40);return utf8(value);}
DWORD child(const std::string& id,const wchar_t* operation) {
    wchar_t executable[32768]{};check(GetModuleFileNameW(nullptr,executable,32768)>0,"module path");
    auto command=std::wstring(L"\"")+executable+L"\" "+operation+L" "+wide(id);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"child launch");CloseHandle(process.hThread);
    auto wait=WaitForSingleObject(process.hProcess,5000);if(wait!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,9);WaitForSingleObject(process.hProcess,1000);CloseHandle(process.hProcess);throw std::runtime_error("child timeout");}
    DWORD code=9;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);return code;
}
}
int wmain(int argc,wchar_t** argv){try {
    if(argc==3) {
        try {ExperimentLock lock(utf8(argv[2]));if(std::wstring(argv[1])==L"abandon")ExitProcess(0);return 0;}
        catch(const std::exception&){return 2;}
    }
    auto id=container();
    {ExperimentLock lock(id);check(child(id,L"try")==2,"cross-process contention not refused");}
    check(child(id,L"try")==0,"lock not released on normal exit");
    {ExperimentLock lock(id);}
    const auto name=experimentLockName(id);
    HANDLE retained=CreateMutexW(nullptr,FALSE,name.c_str());check(retained!=nullptr,"retain mutex");
    check(child(id,L"abandon")==0,"abandon child failed");
    bool refused=false;try{ExperimentLock lock(id);}catch(const std::exception& e){refused=std::string(e.what()).find("abandoned")!=std::string::npos;}
    CloseHandle(retained);check(refused,"abandoned ownership accepted");
    id=container();HANDLE blocker=CreateEventW(nullptr,TRUE,FALSE,experimentLockName(id).c_str());check(blocker!=nullptr,"blocker");
    refused=false;try{ExperimentLock lock(id);}catch(...){refused=true;}CloseHandle(blocker);check(refused,"inaccessible lock accepted");
    auto lower=id;for(auto& c:lower)if(c>='A'&&c<='F')c+=32;check(experimentLockName(id)==experimentLockName(lower),"container case changes mutex identity");
    refused=false;try{ExperimentLock lock("not-a-container");}catch(...){refused=true;}check(refused,"invalid container accepted");
    std::cout<<"Cooperative mutex cross-process contention, normal exit, abandonment, access failure and normalization passed; no HID opens\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
