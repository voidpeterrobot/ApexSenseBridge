#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#include <conio.h>
#include "cli/BridgeOptions.h"
#include "cli/CommandSupport.h"
#include "apex6/live/Live.h"
#include "apex6/RawLibraryContract.h"
#include "capture/WindowsCaptureInput.h"
#include "apex6/experiment/WindowsIo.h"
#include "apex6/experiment/WindowsPulseWait.h"
#include "apex6/experiment/Evidence.h"
#include "platform/PhysicalControllerIsolation.h"
#include "platform/PhysicalInputSource.h"
#include "platform/AudioEndpointProtection.h"
#include "platform/Apex6Settings.h"
#include "platform/GripControl.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace asb::cli {
namespace {
namespace ex=apex6::experiment;
namespace live=apex6::live;
using ex::Time;
void require(bool ok,const std::string& message){if(!ok)throw apex6::ProtocolError(message);}
class OutputScheduling {
    HANDLE task_=nullptr;
public:
    OutputScheduling()=default;
    OutputScheduling(const OutputScheduling&)=delete;
    OutputScheduling& operator=(const OutputScheduling&)=delete;
    void enable() {
        DWORD index=0;
        task_=AvSetMmThreadCharacteristicsW(L"Pro Audio",&index);
        const auto registrationError=GetLastError();
        require(task_!=nullptr,"Cannot register Apex6 output worker with Windows multimedia scheduling: "+std::to_string(registrationError));
        const auto prioritized=AvSetMmThreadPriority(task_,AVRT_PRIORITY_HIGH);
        const auto priorityError=GetLastError();
        require(prioritized!=FALSE,"Cannot set Apex6 multimedia worker priority: "+std::to_string(priorityError));
    }
    ~OutputScheduling(){if(task_)AvRevertMmThreadCharacteristics(task_);}
};
class SessionTrace final:public ex::Trace {
public:
    std::function<void()> progress;
    // Bounded lifecycle evidence. Routine PCM/native packets are never captured.
    std::deque<std::string> events;
    void record(Time time,const std::string& event,std::span<const std::uint8_t> raw,
                std::uint64_t,std::uint64_t)override {
        if(progress)progress();
        if(event.find("mode")==std::string::npos)return;
        if(events.size()==32)events.pop_front();
        events.push_back("{\"time_us\":"+std::to_string(time.count())+",\"event\":"+ex::json(event)+",\"raw\":"+ex::json(ex::hex(raw))+"}");
    }
    bool healthy()const override{return true;}
};
struct NativeObservation {
    Time observed,deadline;
    const char* event;
    std::uint64_t operation;
    std::uint32_t transferred,error,waitMs,waitResult;
    bool waited;
};
struct Pin {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit Pin(const std::filesystem::path& path){handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);require(handle!=INVALID_HANDLE_VALUE,"Cannot pin executable/library against replacement.");}
    ~Pin(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
std::filesystem::path executablePath(){wchar_t path[32768];auto n=GetModuleFileNameW(nullptr,path,32768);require(n&&n<32768,"Cannot resolve engine executable.");return std::wstring(path,n);}
const char* sourceName(live::Stream::Source source){switch(source){
    case live::Stream::Source::Pcm:return "PCM output";
    case live::Stream::Source::Hid:return "HID rumble";
    case live::Stream::Source::Muted:return "muted";
    default:return "ready, awaiting feedback";
}}
}

int commandApex6Bridge(const HidDeviceInfo& selected,const BridgeCommandOptions& options,
    platform::SessionControl* session,platform::GlobalSessionStop& globalStop) {
    std::string error;
    const auto diagnosticStart=live::monotonic();
    const bool dongle=options.apex6DongleDiagnostic||options.apex6DongleBeta;
    const char* sessionScope=options.apex6DongleDiagnostic?"apex6-dongle-live-diagnostic-v1":dongle?"apex6-dongle-beta-v1":"apex6-integrated-beta-v1";
    auto publish=[&](platform::SessionPhase phase,int code,const std::string& message){
        if(session){require(session->publish(phase,code,message,error),error);
            if(phase==platform::SessionPhase::Ready||phase==platform::SessionPhase::Failed)require(session->signalReady(error),error);}
    };
    platform::TemporaryPhysicalControllerIsolation isolation;
    platform::VirtualDualSenseAudioEndpointProtection audio;
    live::RawQueue queue;
    std::unique_ptr<dualsense::VirtualDualSense> backend;
    std::unique_ptr<ex::WindowsTransport> io;
    std::unique_ptr<capture::WindowsCaptureInput> input;
    std::unique_ptr<platform::GripControl> gripControl;
    std::unique_ptr<ex::ExperimentLock> deviceLock;
    std::unique_ptr<Pin> executablePin,libraryPin;
    std::jthread inputThread,supervisor;
    std::atomic_bool orderly=false,finishing=false,supervisoryActive=false;
    std::atomic<double> targetGain=1;
    std::atomic<std::int64_t> inputProgress=live::monotonic().count(),workerProgress=live::monotonic().count(),supervisorProgress=live::monotonic().count();
    SessionTrace trace;live::Result result;
    trace.progress=[&]{workerProgress=live::monotonic().count();};
    // Independent of the DLL input thread and vendor-I/O worker. This remains
    // alive through teardown, including a stalled native close or input join.
    std::jthread watchdog([&](std::stop_token stop){while(!stop.stop_requested()){
        const auto stalled=live::monotonic().count()-workerProgress.load();
        if(stalled>10000000)queue.fail("vendor/session worker stalled");
        if(stalled>20000000){TerminateProcess(GetCurrentProcess(),16);return;}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }});
    std::unique_ptr<live::Stream> stream;
    std::uint64_t nativeEvents=0;
    bool multimediaScheduling=false;
    std::deque<NativeObservation> recentNative;
    auto drainNative=[&]{if(io)io->drainLiveTimings([&](const ex::NativeTiming& t){
        ++nativeEvents;
        if(recentNative.size()==64)recentNative.pop_front();
        recentNative.push_back({t.observed,t.deadline,t.event,t.operation,t.transferred,t.error,t.requestedWaitMs,t.waitResult,t.waitMetadata});
    });};
    std::deque<std::pair<Time,double>> gainChanges;
    std::string executableHash,libraryHash;
    auto cancelled=[&]{return g_stopRequested.load()||queue.failed()||
        (supervisoryActive&&live::monotonic().count()-supervisorProgress.load()>10000000);};
    try {
        require(!options.apexProfileSlot,"Apex6 onboard profiles and adaptive triggers are unsupported; no physical commands were sent.");
        require(!options.xinputIndex&&!options.hapticThresholdExplicit&&!options.verifyVirtualInput,
            "Apex6 uses verified physical input and raw grip output; XInput overrides, summary thresholds and virtual-input diagnostics are unsupported.");
        require(options.virtualBackend!=dualsense::VirtualDualSenseBackend::Sidecar&&options.viiperExecutable.empty(),"Apex6 requires the integrated asb9-or-later libVIIPER; sidecar is unsupported.");
        if(options.apex6Consent&&!options.apex6DongleDiagnostic)platform::updateApex6Settings(platform::kApex6ConsentVersion,std::nullopt);
        auto settings=platform::readApex6Settings();
        require(options.apex6DongleDiagnostic||settings.consentVersion==platform::kApex6ConsentVersion,
            "Apex6 Pro is an integrated grip-only USB beta. Enable Apex6 beta in Tray/Playnite or pass --apex6-beta-consent once. Physical qualification remains pending; faults require operator recovery. Start before launching the game.");
        if(options.apex6DongleDiagnostic)settings.gain=1;
        if(options.gripGain){if(!options.apex6DongleDiagnostic)platform::updateApex6Settings(std::nullopt,options.gripGain);settings.gain=*options.gripGain;}
        targetGain=settings.gain;
        gainChanges.emplace_back(live::monotonic(),settings.gain);
        stream=std::make_unique<live::Stream>(settings.gain,live::Stream::HidLifetime::UntilChanged);
        if(options.sessionToken)gripControl=platform::createGripControl(*options.sessionToken,settings.gain);
        const auto executable=executablePath(),library=executable.parent_path()/L"libVIIPER.dll";
        executablePin=std::make_unique<Pin>(executable);libraryPin=std::make_unique<Pin>(library);
        executableHash=ex::sha256File(executable);libraryHash=ex::sha256File(library);
        auto buildRecord=executable.parent_path()/L"LIBVIIPER-SOURCE.txt";
        if(!std::filesystem::exists(buildRecord))buildRecord=executable.parent_path()/L"Licenses"/L"LIBVIIPER-SOURCE.txt";
        require(std::filesystem::exists(buildRecord)&&std::filesystem::file_size(buildRecord)<=16384,"Apex6 requires the matching LIBVIIPER-SOURCE.txt build record.");
        std::ifstream recordFile(buildRecord,std::ios::binary);
        const std::string record((std::istreambuf_iterator<char>(recordFile)),{});
        require(apex6::matchesRawLibraryContract(record,libraryHash),"Apex6 requires an asb9-or-later DLL matching its recorded SHA-256; repair the package.");
        deviceLock=std::make_unique<ex::ExperimentLock>(ex::utf8(selected.containerId));
        require(isolation.activate(selected,options.sessionToken.value_or(""),std::nullopt,error),error);
        auto binding=ex::inspectInterface(selected);binding.access=ex::AccessMode::Shared;
        ex::GripBaseline baseline;
        if(dongle){
            io=ex::openDongleBaselineTransport(binding,cancelled);
            for(unsigned i=0;i<2;++i){
                ex::Session acquisition(*io,trace);const auto observed=ex::acquireGripBaseline(acquisition);
                if(i)require(observed==baseline,"The two dongle startup baselines differ.");baseline=observed;
                ex::Session separator(*io,trace);const ex::Request uidQuery{4,{},16};
                separator.begin("dongle_startup_uid_boundary",Time(2000000),1,{uidQuery});
                require(ex::uid(separator.exchange(uidQuery))==baseline.unit,"Dongle startup UID changed.");separator.end();
            }
        }else for(unsigned i=0;i<2;++i){
            auto query=ex::openIntegratedBaselineTransport(binding,cancelled);ex::Session acquisition(*query,trace);
            const auto observed=ex::acquireGripBaseline(acquisition);
            require(query->finish()&&query->timingComplete(),"Baseline native finalization failed.");
            if(i)require(observed==baseline,"The two startup baselines differ.");baseline=observed;
        }
        auto entryCheck=[&]{require(!cancelled(),"Apex6 entry cancelled");
            require(ex::sha256File(executable)==executableHash&&ex::sha256File(library)==libraryHash,"Executable/library integrity changed.");};
        live::Policy policy;policy.seconds=0;policy.initialGain=settings.gain;policy.strictDispatch=false;
        supervisorProgress=live::monotonic().count();
        // Hold exclusive access throughout the ready/idle period. Opening this
        // guard sends no reports; its first writes are the fresh entry preflight.
        if(dongle)ex::promoteDongleLiveTransport(*io,baseline,policy,cancelled,entryCheck);
        else io=ex::openIntegratedTransport(baseline,policy,cancelled,entryCheck);
        auto gamepads=capture::captureInputDevices(error);require(error.empty(),error);
        std::erase_if(gamepads,[&](const auto& pad){return pad.containerId!=selected.containerId;});
        require(gamepads.size()==1,"Selected Apex6 must have exactly one gamepad interface in its verified container.");
        input=std::make_unique<capture::WindowsCaptureInput>();
        require(input->open(gamepads.front().instanceId,error),error);
        dualsense::DualSenseInputState initial{};
        require(input->poll(initial,error),
            "Selected Apex6 input is not ready: "+error);
        require(audio.capture(error),error);
        dualsense::VirtualDualSenseOptions virtualOptions;virtualOptions.backend=dualsense::VirtualDualSenseBackend::Integrated;virtualOptions.rawFeedbackSink=&queue;
        backend=dualsense::createVirtualDualSense(virtualOptions);require(backend->open(error),error);
        require(backend->updateInput(initial,error),error);
        require(audio.protectAfterVirtualDualSenseStart(std::chrono::milliseconds(2000),error),error);
        const auto started=live::monotonic().count();inputProgress=started;workerProgress=started;supervisorProgress=started;
        inputThread=std::jthread([&, initial](std::stop_token stop){try {
            dualsense::TouchpadGestureMapper mapper(options.touchpadProfile);auto state=initial;
            while(!stop.stop_requested()&&!cancelled()){
                std::string local;
                if(!input->poll(state,local)){queue.fail("physical input disconnected/failed");break;}
                auto mapped=state;mapper.transform(mapped,std::chrono::steady_clock::now());
                if(!backend->updateInput(mapped,local)){queue.fail("virtual input forwarding failed");break;}
                inputProgress=live::monotonic().count();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }catch(...){queue.fail("input worker exception");}});
        supervisoryActive=true;
        supervisor=std::jthread([&](std::stop_token stop){try {
            auto nextHealth=live::monotonic();
            double savedGain=targetGain.load();
            while(!stop.stop_requested()){
                const auto now=live::monotonic();supervisorProgress=now.count();
                if(now.count()-workerProgress.load()>20000000) {
                    // No speculative recovery traffic from a stalled component.
                    // The native isolation watchdog restores visibility on exit.
                    TerminateProcess(GetCurrentProcess(),16);
                    return;
                }
                if(!finishing&&(now.count()-workerProgress.load()>10000000||now.count()-inputProgress.load()>10000000))queue.fail("session component stalled");
                if(session&&session->stopRequested())orderly=true;
                if(globalStop.stopRequested())orderly=true;
                if(gripControl){if(auto gain=gripControl->poll())targetGain=*gain;}
                if(_kbhit()){
                    const auto key=_getch();const auto gain=targetGain.load();
                    if(key=='q'||key=='Q')orderly=true;
                    else if(key=='+'||key=='=')targetGain=std::min(12.,gain+.5);
                    else if(key=='-')targetGain=std::max(0.,gain-.5);
                    else if(key=='0')targetGain=0;
                    else if(key=='1')targetGain=1;
                }
                const auto gain=targetGain.load();
                if(gain!=savedGain){if(!options.apex6DongleDiagnostic)platform::updateApex6Settings(std::nullopt,gain);savedGain=gain;}
                if(!finishing&&now>=nextHealth){std::string local;if(!isolation.healthy(local)){queue.fail("physical isolation or competing-writer check failed");break;}nextHealth=live::monotonic()+Time(500000);}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }catch(...){queue.fail("session supervisor/control failure");}});
        require(isolation.healthy(error),error);
        require(!cancelled(),queue.failed()?queue.failure():"Apex6 initialization cancelled.");
        // Only the vendor-I/O worker joins MMCSS. Input/control supervision keeps
        // ordinary scheduling, and scope exit restores this thread on every path.
        OutputScheduling outputScheduling;
        outputScheduling.enable();multimediaScheduling=true;
        publish(platform::SessionPhase::Ready,0,dongle?"Apex6 dongle beta ready, awaiting feedback (10 min active / 20 min total). Game launch may continue.":"Apex6 beta ready, awaiting feedback. Game launch may continue.");
        std::cout<<(dongle?"Apex6 DONGLE BETA ready, awaiting feedback. Maximum 10 minutes active / 20 minutes total. ":"Apex6 USB grip beta ready, awaiting feedback. ")<<"Q: orderly stop; +/-: gain; 0: mute; 1: reset. Ctrl+C: fail-stop."<<std::endl;
        auto nextStatus=live::monotonic();auto lastSource=live::Stream::Source::AwaitingFeedback;
        unsigned sourceMessages=0;
        double appliedGain=settings.gain;
        auto pump=[&]{
            workerProgress=live::monotonic().count();
            auto gain=targetGain.load();if(gain!=appliedGain){
                stream->gain(gain);appliedGain=gain;
                if(gainChanges.size()==32)gainChanges.pop_front();
                gainChanges.emplace_back(live::monotonic(),gain);
                std::cout<<"Grip gain target: "<<gain<<" (100 ms ramp)"<<std::endl;
                publish(platform::SessionPhase::Ready,0,std::string("Apex6 beta: ")+sourceName(stream->source(live::monotonic()))+"; gain "+std::to_string(gain));
            }
            std::size_t charge=0;
            while(auto item=queue.pop()){charge+=item->bytes.size()+128;require(charge<=4*1024*1024,"Raw feedback drain exceeded bounded work budget.");stream->ingest(*item,live::monotonic());}
            drainNative();
            const auto now=live::monotonic();
            if(now>=nextStatus){const auto source=stream->source(now);if(source!=lastSource){
                publish(platform::SessionPhase::Ready,0,std::string("Apex6 beta: ")+sourceName(source)+"; gain "+std::to_string(appliedGain));
                if(sourceMessages++<64)std::cout<<sourceName(source)<<"; gain "<<appliedGain<<std::endl;
                else if(sourceMessages==65)std::cout<<"Further source changes remain available in session status; routine console logging is capped.\n";
                lastSource=source;
            }nextStatus=now+Time(1000000);}
        };
        const auto readyAt=live::monotonic();
        auto stop=[&]{return orderly.load()||(options.duration&&live::monotonic()-readyAt>=*options.duration)||
            (dongle&&live::monotonic()-diagnosticStart>=Time(1190000000));};
        while(!stop()){
            require(!cancelled(),queue.failed()?queue.failure():"Session cancelled; no recovery commands sent.");pump();
            // Gain does not affect eligibility. Silence and explicit zero may
            // update state but never enter grip mode while awaiting feedback.
            if(stream->eligible(live::monotonic()))break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(stop()){result.complete=true;result.stopReason="stopped_before_entry";}
        else {
            // Fresh preflight is inside run(), immediately before grip entry.
            struct CancelEvent {HANDLE handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);~CancelEvent(){if(handle)CloseHandle(handle);}} cancelEvent;
            require(cancelEvent.handle!=nullptr,"Cannot create native wait event.");
            ex::WindowsPulseWait wait(cancelEvent.handle,[&]{return io->now();});
            live::Control control{[&](Time due){wait.waitUntil(due);},cancelled,stop,pump,
                [&](Time now){return stream->packet(now);},[&]{pump();stream->discardPcm();},
                [&](Time now){io->stopLive(now);},[&]{workerProgress=live::monotonic().count();}};
            result=live::run(*io,trace,baseline,policy,control,entryCheck,dongle?live::ReplyBoundary::DongleLiveDiagnostic:live::ReplyBoundary::None);
        }
    } catch(const std::exception& e){result.complete=false;result.failure=e.what();result.stopReason="failure";}
    finishing=true;
    workerProgress=live::monotonic().count();
    if(inputThread.joinable()){inputThread.request_stop();inputThread.join();}
    input.reset();
    if(supervisor.joinable()){supervisor.request_stop();supervisor.join();}
    if(stream)stream->reset();
    if(io){if(!io->finish()||!io->timingComplete()){result.complete=false;result.failure+="; native I/O finalization failed";}
        try{drainNative();}catch(...){result.complete=false;result.failure+="; native timing collection failed";}io.reset();}
    if(backend){
        if(backend->connected()&&!backend->updateInput({},error)){result.complete=false;result.failure+="; virtual neutral input failed";}
        backend->close();backend.reset();
    }
    if(queue.failed()){result.complete=false;result.failure+=std::string("; ")+queue.failure();}
    if(audio.captured()&&!audio.protectAfterVirtualDualSenseStart(std::chrono::milliseconds(250),error)){result.complete=false;result.failure+="; "+error;}
    if(!isolation.restore(error)){result.complete=false;result.failure+="; isolation restore failed: "+error;}
    result.integratedBeta=!options.apex6DongleDiagnostic;result.strictDispatch=false;
    std::ostringstream telemetry;telemetry<<"{\"schema\":1,\"scope\":"<<ex::json(sessionScope)<<",\"integrated_beta\":"<<(options.apex6DongleDiagnostic?"false":"true")<<",\"physical_qualification\":\"pending\",\"executable_sha256\":"<<ex::json(executableHash)<<",\"library_sha256\":"<<ex::json(libraryHash)<<",\"gain\":"<<targetGain.load()<<",\"native_events\":"<<nativeEvents<<",\"output_scheduling\":"<<ex::json(multimediaScheduling?"mmcss_pro_audio_high":"not_registered");
    if(stream){const auto& m=stream->metrics();auto q=queue.metrics();telemetry<<",\"raw_records\":"<<q.records<<",\"peak_queue_bytes\":"<<q.peakBytes<<",\"pcm_records\":"<<m.validRecords<<",\"hid_accepted\":"<<m.hidAccepted<<",\"hid_samples\":"<<m.hidSamples<<",\"dropped_pcm\":"<<m.droppedSamples<<",\"stale_records\":"<<m.staleRecords<<",\"clipped_samples\":"<<m.strength.clippedSamples<<",\"peak_before_limit\":"<<m.strength.peakBeforeLimit<<",\"peak_after_limit\":"<<m.strength.peakAfterLimit<<",\"applied_gain\":"<<stream->gain();}
    telemetry<<",\"recent_gain_targets\":[";
    for(std::size_t i=0;i<gainChanges.size();++i){if(i)telemetry<<',';telemetry<<"{\"time_us\":"<<gainChanges[i].first.count()<<",\"gain\":"<<gainChanges[i].second<<'}';}telemetry<<']';
    telemetry<<",\"recent_native_timing\":[";
    for(std::size_t i=0;i<recentNative.size();++i){if(i)telemetry<<',';const auto& t=recentNative[i];
        telemetry<<"{\"time_us\":"<<t.observed.count()<<",\"event\":"<<ex::json(t.event)<<",\"operation\":"<<t.operation<<",\"transferred\":"<<t.transferred<<",\"error\":"<<t.error;
        if(t.waited)telemetry<<",\"deadline_us\":"<<t.deadline.count()<<",\"wait_ms\":"<<t.waitMs<<",\"wait_result\":"<<t.waitResult;
        telemetry<<'}';}telemetry<<']';
    auto lifecycle=live::resultJson(result);
    if(dongle){const std::string oldScope=result.integratedBeta?"apex6-integrated-beta-v1":live::scope;const auto pos=lifecycle.find(oldScope);if(pos!=std::string::npos)lifecycle.replace(pos,oldScope.size(),sessionScope);}
    telemetry<<",\"lifecycle\":"<<lifecycle<<",\"mode_evidence\":[";
    for(std::size_t i=0;i<trace.events.size();++i){if(i)telemetry<<',';telemetry<<trace.events[i];}telemetry<<"]}\n";
    try {
        auto path=options.telemetryJson;
        if(path.empty()) {
            wchar_t local[32768];const auto size=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
            require(size&&size<32768,"Cannot locate Apex6 telemetry directory.");
            path=std::filesystem::path(local)/L"ApexSenseBridge"/L"Logs"/L"apex6-last-session.json";
        }
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        auto temporary=path;temporary+=L".tmp";
        {std::ofstream out(temporary);out<<telemetry.str();out.flush();require(bool(out),"Telemetry write failed.");}
        require(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Telemetry replacement failed.");
    }catch(const std::exception& e){result.complete=false;result.failure+=std::string("; ")+e.what();}
    const auto message=result.complete?"Apex6 beta stopped. Restore replies remain unverified evidence.":"Apex6 recovery required: "+result.failure;
    try{publish(result.complete?platform::SessionPhase::Stopped:platform::SessionPhase::Failed,result.complete?0:16,message);}catch(...){result.complete=false;}
    std::cout<<message<<'\n';return result.complete?0:16;
}
}
