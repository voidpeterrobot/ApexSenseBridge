#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dualsense/VirtualDualSense.h"
#include "apex6/live/Stream.h"
#include <thread>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("fake DLL required");
    const auto path=std::filesystem::absolute(argv[1]);const auto module=LoadLibraryW(path.c_str());
    auto configure=reinterpret_cast<void(*)(unsigned)>(GetProcAddress(module,"ConfigureCaptureFake"));
    auto created=reinterpret_cast<unsigned(*)()>(GetProcAddress(module,"CaptureFakeCreated"));
    if(!configure||!created)throw std::runtime_error("fake exports missing");
    for(unsigned mode=0;mode<7;++mode){
        configure(mode);asb::apex6::live::RawQueue queue;
        asb::dualsense::VirtualDualSenseOptions options;options.viiperLibrary=path;options.rawFeedbackSink=&queue;
        auto backend=asb::dualsense::createVirtualDualSense(options);std::string error;
        const auto opened=backend->open(error);
        if(opened!=(mode==0))throw std::runtime_error("raw startup/ABI rejection: "+error);
        if(mode==1&&created()!=0)throw std::runtime_error("ABI mismatch created virtual resources");
        if(opened){std::this_thread::sleep_for(std::chrono::milliseconds(20));if(!backend->updateInput({},error))throw std::runtime_error(error);}
        backend->close();const auto records=queue.metrics().records;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if(records!=queue.metrics().records)throw std::runtime_error("callback escaped close barrier");
        if(opened&&(records<2||queue.failed()))throw std::runtime_error("raw callback ownership failed");
    }
    asb::apex6::live::RawQueue queue;asb::dualsense::VirtualDualSenseOptions options;
    options.backend=asb::dualsense::VirtualDualSenseBackend::Sidecar;options.rawFeedbackSink=&queue;
    auto backend=asb::dualsense::createVirtualDualSense(options);std::string error;
    if(backend->open(error))throw std::runtime_error("raw sidecar accepted");
    std::cout<<"Integrated raw callback/ABI/close tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
