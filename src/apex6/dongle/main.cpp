#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#include <conio.h>
#include "apex6/dongle/Neutral.h"
#include "apex6/experiment/WindowsIo.h"
#include "apex6/experiment/WindowsPulseWait.h"
#include "apex6/experiment/Evidence.h"
#include "capture/WindowsCaptureInput.h"
#include "core/ControllerCapabilities.h"
#include "platform/PhysicalControllerIsolation.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <thread>

namespace ex=asb::apex6::experiment;
namespace live=asb::apex6::live;
namespace dongle=asb::apex6::dongle;
using Isolation=asb::platform::TemporaryPhysicalControllerIsolation;
namespace {
HANDLE cancelEvent=nullptr;
BOOL WINAPI cancelled(DWORD) { if(cancelEvent)SetEvent(cancelEvent);return TRUE; }
void require(bool ok,const std::string& why) {if(!ok)throw asb::apex6::ProtocolError(why);}
void writeNew(const std::filesystem::path& path,const std::string& text) {
    require(!std::filesystem::exists(path),"Evidence file already exists");
    std::ofstream file(path,std::ios::binary);file<<text;file.close();require(bool(file),"Evidence write failed");
}
struct Scheduling {
    HANDLE task=nullptr;
    void enable(){DWORD index=0;task=AvSetMmThreadCharacteristicsW(L"Pro Audio",&index);require(task!=nullptr,"MMCSS registration failed");require(AvSetMmThreadPriority(task,AVRT_PRIORITY_HIGH)!=FALSE,"MMCSS priority failed");}
    ~Scheduling(){if(task)AvRevertMmThreadCharacteristics(task);}
};
}
int main(int argc,char** argv) {
    // Native isolation recovery invokes this same executable. It must work even
    // if the diagnostic crashed or no controller is presently connected.
    if(argc==3&&std::string(argv[1])=="hidhide-watchdog") {
        std::uint32_t owner=0;const std::string value=argv[2];const auto parsed=std::from_chars(value.data(),value.data()+value.size(),owner);
        if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||!owner)return 1;
        std::string error;const auto code=Isolation::watchAndRecover(owner,"",error);if(!error.empty())std::cerr<<error<<'\n';return code;
    }
    if(argc==2&&std::string(argv[1])=="restore-controller-visibility") {
        bool restored=false;std::string error;if(!Isolation::recoverPending(restored,error)){std::cerr<<error<<'\n';return 1;}return 0;
    }
    if(argc!=6||(std::string(argv[1])!="neutral"&&std::string(argv[1])!="neutral-boundaries"&&std::string(argv[1])!="pulse-left"&&std::string(argv[1])!="pulse-right"&&std::string(argv[1])!="pulse-both"&&std::string(argv[1])!="readback")||std::string(argv[2])!="--dongle-confirmed"||
        std::string(argv[3])!="--supervised"||std::string(argv[4])!="--output") {
        std::cerr<<"Usage: ApexSenseBridgeApex6DongleExperiment neutral|neutral-boundaries|pulse-left|pulse-right|pulse-both|readback --dongle-confirmed --supervised --output NEW_DIRECTORY\n"
            <<"Scope: dongle diagnostic; neutral or fixed 256-ms low-strength grip pulse; fresh baselines.\n"
            <<"Operator must observe throughout, close games/writers, and be ready to power off on failure. No legacy approval files accepted.\n";
        return 1;
    }
    const std::filesystem::path output=argv[5];
    const bool readbackOnly=std::string(argv[1])=="readback";
    const auto side=std::string(argv[1])=="pulse-left"?dongle::PulseSide::Left:std::string(argv[1])=="pulse-right"?dongle::PulseSide::Right:std::string(argv[1])=="pulse-both"?dongle::PulseSide::Both:dongle::PulseSide::None;
    const bool pulse=side!=dongle::PulseSide::None;
    const auto maximumWaves=pulse?dongle::pulseMaximumWaves:dongle::maximumWaves;
    const auto boundary=pulse||std::string(argv[1])=="neutral-boundaries"?live::ReplyBoundary::DongleUidDiagnostic:live::ReplyBoundary::None;
    const auto evidenceScope=pulse?dongle::pulseScope(side):boundary==live::ReplyBoundary::None?dongle::scope:dongle::boundaryScope;
    try {require(!std::filesystem::exists(output),"Output directory must be new");}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    cancelEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!cancelEvent)return 1;
    SetConsoleCtrlHandler(cancelled,TRUE);
    const auto start=live::monotonic();
    std::jthread watchdog([&](std::stop_token stop){while(!stop.stop_requested()){
        if(live::monotonic()-start>ex::Time(90000000)){TerminateProcess(GetCurrentProcess(),16);return;}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }});
    Isolation isolation;std::unique_ptr<ex::WindowsTransport> io;
    std::unique_ptr<ex::ExperimentLock> lock;
    std::unique_ptr<ex::Evidence> evidence;
    std::unique_ptr<asb::capture::WindowsCaptureInput> input;
    std::atomic_bool fault=false,orderly=false;
    std::jthread monitor;
    std::string healthFailure,restoreError;
    live::Result result;
    result.strictDispatch=false;
    bool nativeFinished=true,evidenceFinished=true,visibilityRestored=false;
    unsigned startupQueries=0,baselinesCompleted=0;
    auto isCancelled=[&]{return fault.load()||WaitForSingleObject(cancelEvent,0)!=WAIT_TIMEOUT;};
    try {
        evidence=std::make_unique<ex::Evidence>(output);
        wchar_t image[32768];const auto length=GetModuleFileNameW(nullptr,image,32768);
        require(length>0&&length<32768,"Cannot locate diagnostic executable");
        const std::filesystem::path executable=std::wstring(image,length);
        const auto imageHash=ex::sha256File(executable);
        writeNew(output/"experiment.json",std::string("{\"scope\":\"")+evidenceScope+"\",\"operator_confirmed_transport\":\"dongle\",\"supervised\":true,\"executable_sha256\":\""+imageHash+"\",\"maximum_waveforms\":"+std::to_string(maximumWaves)+",\"nonzero_samples_allowed\":"+(pulse?"true":"false")+",\"peak_sample\":"+(pulse?"0.0625":"0")+"}\n");
        auto devices=ex::vendorInterfaces();std::erase_if(devices,[](const auto& d){return !asb::isApex6Vendor(d);});
        require(devices.size()==1,"Exactly one eligible Apex6 controller required");
        const auto selected=devices.front();lock=std::make_unique<ex::ExperimentLock>(ex::utf8(selected.containerId));
        std::string error;require(isolation.activate(selected,"",std::nullopt,error),error);
        auto binding=ex::inspectInterface(selected);binding.access=ex::AccessMode::Shared;
        ex::GripBaseline baseline;
        io=ex::openDongleBaselineTransport(binding,isCancelled);
        for(unsigned index=0;index<2;++index) {
            ex::Session session(*io,*evidence);
            ex::GripBaseline current;
            try {current=ex::acquireGripBaseline(session);}
            catch(...){startupQueries+=session.queryAttempts();throw;}
            startupQueries+=session.queryAttempts();
            if(index)require(current==baseline,"Two fresh baselines differ");baseline=current;
            ++baselinesCompleted;
            writeNew(output/(index?"baseline-b.asb":"baseline-a.asb"),ex::encodeGripBaseline(current));
            ex::Session boundary(*io,*evidence);const ex::Request uidQuery{4,{},16};
            try {
                boundary.begin("dongle_uid_boundary",ex::Time(2000000),1,{uidQuery});
                require(ex::uid(boundary.exchange(uidQuery))==baseline.unit,"UID changed at baseline boundary");
                boundary.end();
            }catch(...){startupQueries+=boundary.queryAttempts();throw;}
            startupQueries+=boundary.queryAttempts();
        }
        if(readbackOnly){result.complete=true;result.stopReason="query_only";}
        else {
        auto pads=asb::capture::captureInputDevices(error);require(error.empty(),error);
        std::erase_if(pads,[&](const auto& pad){return pad.containerId!=selected.containerId;});
        require(pads.size()==1,"Selected container must have exactly one gamepad");
        input=std::make_unique<asb::capture::WindowsCaptureInput>();require(input->open(pads.front().instanceId,error),error);
        asb::dualsense::DualSenseInputState state{};require(input->poll(state,error),error);
        auto authorize=[&]{require(!isCancelled(),"Dongle diagnostic cancelled or health failed");require(ex::sha256File(executable)==imageHash,"Diagnostic executable changed");std::string status;require(isolation.healthy(status),status);};
        if(pulse)ex::promoteDonglePulseTransport(*io,baseline,isCancelled,authorize,side);
        else ex::promoteDongleNeutralTransport(*io,baseline,isCancelled,authorize,boundary);
        monitor=std::jthread([&](std::stop_token stop){try{
            auto nextHealth=live::monotonic();
            while(!stop.stop_requested()&&!isCancelled()) {
                std::string status;if(!input->poll(state,status)){healthFailure="Input link failed: "+status;fault=true;break;}
                if(live::monotonic()>=nextHealth){if(!isolation.healthy(status)){healthFailure=status;fault=true;break;}nextHealth=live::monotonic()+ex::Time(250000);}
                if(_kbhit()){const auto key=_getch();if(key=='q'||key=='Q')orderly=true;}
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }catch(const std::exception& e){healthFailure=e.what();fault=true;}});
        Scheduling scheduling;scheduling.enable();ex::WindowsPulseWait wait(cancelEvent,[&]{return io->now();});
        live::Control control{[&](ex::Time due){wait.waitUntil(due);},isCancelled,[&]{return orderly.load();},[]{},
            [](ex::Time){return asb::apex6::gripWaveform({});},[]{},[&](ex::Time now){io->stopLive(now);}};
        if(pulse)std::cout<<"Starting fixed 256-ms low-strength "<<dongle::pulseName(side)<<" grip pulse. ";
        else std::cout<<"Starting dongle neutral lifecycle: no intentional vibration. ";
        std::cout<<"Observe controller. Q stops orderly; Ctrl+C fails stop.\n"<<std::flush;
        result=pulse?dongle::runPulse(*io,*evidence,baseline,control,authorize,side):dongle::runNeutral(*io,*evidence,baseline,control,authorize,boundary);
        }
    } catch(const std::exception& e){result.failure=e.what();result.stopReason="failure";result.complete=false;}
    if(monitor.joinable()){monitor.request_stop();monitor.join();}
    if(!healthFailure.empty()){result.complete=false;result.failure+="; "+healthFailure;}
    if(io){
        nativeFinished=io->finish()&&io->timingComplete();
        try {io->drainLiveTimings([&](const ex::NativeTiming& t){evidence->record(t.observed,std::string("native_")+t.event,std::span(t.raw).first(t.rawSize),t.operation,(std::uint64_t(t.error)<<32)|t.transferred);});}
        catch(const std::exception& e){nativeFinished=false;result.failure+="; "+std::string(e.what());}
    }
    io.reset();input.reset();visibilityRestored=isolation.restore(restoreError);
    if(evidence)evidenceFinished=evidence->finish();
    try {
        auto text=live::resultJson(result);const auto position=text.find(live::scope);if(position!=std::string::npos)text.replace(position,std::string(live::scope).size(),evidenceScope);
        writeNew(output/"result.json",text);
        writeNew(output/"finalization.json",std::string("{\"startup_readback_queries\":")+std::to_string(startupQueries)+",\"baselines_completed\":"+std::to_string(baselinesCompleted)+",\"native_complete\":"+(nativeFinished?"true":"false")+",\"evidence_complete\":"+(evidenceFinished?"true":"false")+",\"visibility_restored\":"+(visibilityRestored?"true":"false")+",\"restore_error\":"+ex::json(restoreError)+"}\n");
        std::cout<<text;
        std::cout<<"Startup readback queries: "<<startupQueries<<"; complete baselines: "<<baselinesCompleted<<"\n";
    }catch(const std::exception& e){evidenceFinished=false;std::cerr<<e.what()<<'\n';}
    const bool complete=result.complete&&(readbackOnly||(result.postflightMatches&&(result.waves==maximumWaves||result.stopReason=="operator_q")))&&nativeFinished&&evidenceFinished&&visibilityRestored;
    std::cout<<(complete?(readbackOnly?"Two baseline acquisitions matched on one exclusive handle. Zero actuator commands.\n":"Diagnostic sequence complete. Physical recovery remains UNVERIFIED; report normal input/vibration and any unexpected movement.\n"):
        (result.deviceStateUncertain?"Diagnostic failed after mode/output uncertainty. Power off/restart controller. No retry or speculative cleanup.\n":"Diagnostic stopped before actuator output. Visibility restoration is recorded separately. No automatic retry.\n"));
    watchdog.request_stop();watchdog.join();SetConsoleCtrlHandler(cancelled,FALSE);CloseHandle(cancelEvent);cancelEvent=nullptr;
    return complete?0:1;
}
