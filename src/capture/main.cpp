#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include "capture/WindowsCaptureBackend.h"
#include "capture/FeedbackRecorder.h"
#include "capture/CaptureVerifier.h"
#include "capture/WindowsCaptureInput.h"
#include "capture/WindowsFixturePlayer.h"
#include "platform/AudioEndpointProtection.h"
#include <charconv>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace {
std::atomic_bool stop{false};
BOOL WINAPI consoleHandler(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT) { stop=true; return TRUE; }
    return FALSE;
}
void usage() {
    std::cerr << "ApexSenseBridgeCapture record --input synthetic --output NEW_DIR [--seconds 60] [--demo] [--library PATH]\n"
                 "ApexSenseBridgeCapture verify --capture DIR --fixture SIGNAL_WAV\n"
                 "ApexSenseBridgeCapture list-inputs --json\n"
                 "ApexSenseBridgeCapture record --input-device EXACT_INSTANCE_ID --output NEW_DIR [--seconds 60]\n"
                 "ApexSenseBridgeCapture input-status --input-device EXACT_INSTANCE_ID [--seconds 5]\n"
                 "Record --seconds 0 captures continuously until Q or Ctrl+C.\n"
                 "Isolated experimental raw capture; no physical actuator support.\n";
}
}
int run(int argc,char** argv) {
    using namespace asb::capture;
    if(argc==2 && std::string(argv[1])=="--help") { usage(); return 0; }
    if(argc==3 && std::string(argv[1])=="list-inputs" && std::string(argv[2])=="--json") {
        std::string error; const auto devices=captureInputDevices(error);
        if(!error.empty()) { std::cerr << error << '\n'; return 2; }
        std::cout << '['; bool first=true;
        for(const auto& d:devices) {
            if(!first) std::cout << ','; first=false;
            std::cout << "{\"instance_id\":" << jsonString(utf8(d.instanceId)) << ",\"container_id\":" << jsonString(utf8(d.containerId))
                      << ",\"input_bytes\":" << d.inputReportLength << '}';
        }
        std::cout << "]\n"; return 0;
    }
    if(argc==6 && std::string(argv[1])=="verify" && std::string(argv[2])=="--capture" && std::string(argv[4])=="--fixture") {
        std::string report,error;
        if(!verifyCapture(wide(argv[3]),wide(argv[5]),report,error)) { std::cerr << error << '\n'; return 3; }
        std::cout << report << '\n'; return 0;
    }
    const bool inputStatus=argc>=2 && std::string(argv[1])=="input-status";
    if(argc<2 || (!inputStatus && std::string(argv[1])!="record")) { usage(); return 1; }
    Limits limits; std::filesystem::path output,library,fixture;
    if(inputStatus) limits.seconds=5;
    std::string inputDevice;
    bool synthetic=false,demo=false;
    for(int i=2;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--demo") { demo=true; continue; }
        if(i+1==argc) { usage(); return 1; }
        const std::string value=argv[++i];
        if(arg=="--input" && value=="synthetic") synthetic=true;
        else if(arg=="--input-device") inputDevice=value;
        else if(arg=="--output") output=std::filesystem::path(std::u8string(value.begin(),value.end()));
        else if(arg=="--library") library=std::filesystem::path(std::u8string(value.begin(),value.end()));
        else if(arg=="--fixture") fixture=std::filesystem::path(std::u8string(value.begin(),value.end()));
        else if(arg=="--seconds") {
            const auto result=std::from_chars(value.data(),value.data()+value.size(),limits.seconds);
            if(result.ec!=std::errc{} || result.ptr!=value.data()+value.size() || limits.seconds>3600 || (inputStatus&&!limits.seconds)) {
                std::cerr << "record seconds must be 0..3600; input-status requires 1..3600\n"; return 1;
            }
        } else { usage(); return 1; }
    }
    limits.continuous=!limits.seconds;
    if(synthetic==!inputDevice.empty() || (!inputStatus && output.empty()) || (inputStatus && synthetic) || (demo && !synthetic)) { usage(); return 1; }
    try {
        if(!fixture.empty()) {
            const auto pcm=loadFixturePcm(fixture);
            if(!limits.continuous&&limits.seconds<10+pcm.size()/384000) throw std::runtime_error("capture duration must allow fixture duration plus 10 seconds");
            if(!synthetic) throw std::runtime_error("controlled fixture playback requires synthetic input mode");
        }
        WindowsCaptureInput physicalInput;
        std::string inputError;
        if(!synthetic && !physicalInput.open(wide(inputDevice),inputError)) { std::cerr << inputError << '\n'; return 2; }
        if(inputStatus) {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(limits.seconds);
            std::cout << jsonString(physicalInput.description()) << '\n';
            asb::dualsense::DualSenseInputState state{},previous{}; bool first=true;
            while(std::chrono::steady_clock::now()<end) {
                if(!physicalInput.poll(state,inputError)) { std::cerr << inputError << '\n'; return 3; }
                if(first || state!=previous) {
                    std::cout << "{\"lx\":" << +state.lx << ",\"ly\":" << +state.ly << ",\"rx\":" << +state.rx << ",\"ry\":" << +state.ry
                              << ",\"l2\":" << +state.l2 << ",\"r2\":" << +state.r2 << ",\"dpad\":" << +state.dpad << ",\"buttons\":" << state.buttons << "}\n";
                    first=false;previous=state;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            return 0;
        }
        if(library.empty()) {
            std::wstring executable(32768,L'\0');
            const auto count=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
            if(!count || count>=executable.size()) throw std::runtime_error("cannot locate executable");
            executable.resize(count); library=std::filesystem::path(executable).parent_path()/"libVIIPER.dll";
        }
        const auto hash=sha256File(library);
        FeedbackRecorder recorder(output,limits); std::string error;
        if(!recorder.start(error)) { std::cerr << error << '\n'; return 2; }
        recorder.metadata("application","ApexSenseBridgeCapture " ASB_VERSION_STRING " / capture ABI 1");
        SYSTEMTIME utc{};GetSystemTime(&utc);
        std::ostringstream startedUtc; startedUtc << std::setfill('0') << std::setw(4) << utc.wYear << '-'
            << std::setw(2) << utc.wMonth << '-' << std::setw(2) << utc.wDay << 'T' << std::setw(2) << utc.wHour
            << ':' << std::setw(2) << utc.wMinute << ':' << std::setw(2) << utc.wSecond << 'Z';
        recorder.metadata("started_utc",startedUtc.str());
        asb::platform::VirtualDualSenseAudioEndpointProtection audio;
        WindowsCaptureBackend backend(recorder);
        WindowsFixturePlayer player;
        bool ready=audio.capture(error);
        const auto defaultsBefore=audio.defaultEndpointIds();
        for(std::size_t i=0;i<3;++i) recorder.metadata("audio_before_role_"+std::to_string(i),utf8(defaultsBefore[i]));
        if(!ready) recorder.fail("cannot snapshot default audio roles");
        if(ready) ready=backend.open(library,error);
        if(ready && !audio.protectAfterVirtualDualSenseStart(std::chrono::milliseconds(2000),error)) {
            recorder.fail("default audio protection failed"); ready=false;
        }
        SetConsoleCtrlHandler(consoleHandler,TRUE);
        if(ready) {
            recorder.metadata("virtual_serial",backend.serial());
            if(!fixture.empty()) {
                recorder.metadata("fixture_sha256",sha256File(fixture));
                player.start(fixture,backend.serial(),recorder);
            }
            std::cout << "Virtual DualSense ready: " << backend.serial() << ". Recording original host feedback.\n"
                      << "Physical output is unavailable. Q finishes capture; Ctrl+C cancels.\n";
            const auto start=std::chrono::steady_clock::now();
            asb::dualsense::DualSenseInputState input{};
            while(!stop && !recorder.failed() && !backend.disconnected()) {
                const auto elapsed=std::chrono::steady_clock::now()-start;
                if(!limits.continuous&&elapsed>=std::chrono::seconds(limits.seconds)) break;
                if(_kbhit()){const int key=_getch();if(key=='q'||key=='Q'){recorder.metadata("stop_reason","operator_q");break;}}
                if(!synthetic && !physicalInput.poll(input,error)) { recorder.fail("physical input source lost"); break; }
                if(synthetic) input={};
                if(demo && (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()%2)==1) {
                    input.buttons=asb::dualsense::button::kCross; input.lx=192; input.l2=64;
                }
                if(!backend.update(input,error)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            asb::dualsense::DualSenseInputState neutral{};
            backend.update(neutral,error);
        }
        if(stop) recorder.fail("capture cancelled by operator");
        if(!fixture.empty()) {
            if(ready && !player.done()) recorder.fail("capture ended before fixture playback completed");
            player.stop();
            recorder.metadata("fixture_endpoint",player.endpoint());
            recorder.metadata("fixture_error",player.error());
        }
        recorder.metadata("audio_default_protection",asb::platform::audioDefaultProtectionStatusName(audio.status()));
        recorder.metadata("session_error",error.substr(0,1024));
        if(!error.empty()) std::cerr << error << '\n';
        // Recheck immediately before detach too, including cancelled/error runs.
        if(audio.captured() && !audio.protectAfterVirtualDualSenseStart(std::chrono::milliseconds(250),error))
            recorder.fail("default audio protection failed during cleanup");
        backend.close(error);
        asb::platform::VirtualDualSenseAudioEndpointProtection afterAudio;
        if(audio.captured()) {
            if(!afterAudio.capture(error)) recorder.fail("cannot verify default audio roles after detach");
            const auto defaultsAfter=afterAudio.defaultEndpointIds();
            for(std::size_t i=0;i<3;++i) recorder.metadata("audio_after_role_"+std::to_string(i),utf8(defaultsAfter[i]));
            if(defaultsBefore!=defaultsAfter) recorder.fail("default audio roles differ after detach; review retained endpoint IDs");
        }
        const bool complete=recorder.finish(synthetic?(demo?"synthetic-demo":"synthetic-neutral"):physicalInput.description(),hash,error);
        SetConsoleCtrlHandler(consoleHandler,FALSE);
        std::cout << (complete?"Capture complete: ":"Capture incomplete: ") << output.string() << '\n';
        if(!complete) std::cerr << error << '\n';
        return complete?0:3;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}

// Windows supplies UTF-16 arguments; do not interpret an ANSI argv as UTF-8.
int wmain(int argc,wchar_t** argv) {
    try {
        std::vector<std::string> values; values.reserve(argc);
        for(int i=0;i<argc;++i) values.push_back(asb::capture::utf8(argv[i]));
        std::vector<char*> pointers; pointers.reserve(argc);
        for(auto& value:values) pointers.push_back(value.data());
        return run(argc,pointers.data());
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
