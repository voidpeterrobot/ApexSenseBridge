#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#include "apex6/live/Live.h"
#include "apex6/experiment/WindowsIo.h"
#include "apex6/experiment/Evidence.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6/experiment/WindowsPulseWait.h"
#include "capture/FeedbackRecorder.h"
#include "capture/WindowsCaptureBackend.h"
#include "capture/WindowsCaptureInput.h"
#include "capture/WindowsFixturePlayer.h"
#include "capture/CaptureVerifier.h"
#include "platform/AudioEndpointProtection.h"
#include "BuildIdentity.h"
#include "apex6_rehearsal_fixture.h"
#include "apex6_live_fixture.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <thread>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <limits>

namespace ex=asb::apex6::experiment;
namespace live=asb::apex6::live;
namespace cap=asb::capture;
using namespace asb::apex6;
using ex::Time;
namespace {
void require(bool b,const char* why){if(!b)throw ProtocolError(why);}
struct Handle {HANDLE h=nullptr;~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle()=default;Handle(const Handle&)=delete;};
struct Shared {volatile LONG gainHalf=2,revision=0,orderly=0,ready=0,finishing=0;alignas(8) volatile LONG64 workerProgress=0,supervisorProgress=0;};
void progress(Shared& s){InterlockedExchange64(&s.workerProgress,static_cast<LONG64>(GetTickCount64()));}
bool stalled(volatile LONG64& stamp,ULONGLONG limit){const auto last=static_cast<ULONGLONG>(InterlockedCompareExchange64(&stamp,0,0));return GetTickCount64()-last>=limit;}
struct Mapping {Shared* data=nullptr;~Mapping(){if(data)UnmapViewOfFile(data);}};
std::atomic<HANDLE> cancelHandle=nullptr;
BOOL WINAPI consoleHandler(DWORD event){if(event==CTRL_C_EVENT||event==CTRL_BREAK_EVENT||event==CTRL_CLOSE_EVENT){auto h=cancelHandle.load();if(h){SetEvent(h);return TRUE;}}return FALSE;}
std::int64_t unixNow(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::filesystem::path executable(){std::wstring p(32768,0);auto n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size()));require(n&&n<p.size(),"cannot resolve executable");p.resize(n);return p;}
std::string hash(const std::string& s){return ex::sha256({reinterpret_cast<const std::uint8_t*>(s.data()),s.size()});}
std::string read(const std::filesystem::path& p){require(std::filesystem::is_regular_file(p)&&std::filesystem::file_size(p)<=65536,"expected bounded regular input");std::ifstream in(p,std::ios::binary);std::string s((std::istreambuf_iterator<char>(in)),{});require(!in.bad(),"read failed");return s;}
void writeNew(const std::filesystem::path& p,const std::string& text){Handle file;file.h=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);require(file.h!=INVALID_HANDLE_VALUE,"output must be a new file");DWORD n=0;require(text.size()<0xffffffffu&&WriteFile(file.h,text.data(),static_cast<DWORD>(text.size()),&n,nullptr)&&n==text.size()&&FlushFileBuffers(file.h),"evidence write failed");}
using Options=std::map<std::wstring,std::wstring>;
std::wstring get(const Options& o,const wchar_t* name){auto it=o.find(name);require(it!=o.end(),"required option missing");return it->second;}
unsigned integer(const std::wstring& v){std::size_t n=0;auto x=std::stoul(v,&n);require(n==v.size()&&x<=1000000,"invalid integer");return static_cast<unsigned>(x);}
void only(const Options& o,std::initializer_list<const wchar_t*> allowed){for(const auto& [key,value]:o)require(std::any_of(allowed.begin(),allowed.end(),[&](auto a){return key==a;}),"unexpected option");}
const std::string contract=std::string("gain_range=0..12;ceiling=0.75;ramp_samples=100;raw_bytes=4194304;record_bytes=1048576;age_us=40000;spacing_us=8000;write_us=4000;timed_pcm_wait_s=120;timed_worker_s=240;timed_supervisor_s=245;timed_stream_packets=7500;seconds_0=continuous_until_q;continuous_progress_watchdog_s=10,15;continuous_evidence=disk_until_io_failure;continuous_authorization=activate_once_while_fresh;lead=1;tail=2;tail_hold_us=8000;modes=4;queries=28;fail_stop=no_cleanup;raw=48000x4xs16;channels=2,3;resample=48:1;restore=captured_anomaly_unverified;hid=usb02_48or63or64_valid_compatible_select;hid_left=large_80Hz;hid_right=small_160Hz;hid_peak=0.125;hid_lease_us=2000000;pcm_priority_us=100000;pcm_active_s16=64;isolation=lease5s_strict_hidhide;vendor=exclusive")+live::dispatchContract;
struct Review {
    ex::GripBaseline baseline;live::Policy policy;std::int64_t prepared=0;
    std::string source,exe,libraryHash,library,input,fixture,fixtureHash,isolationLease,isolationToken;
};
std::string encode(const Review& r){std::ostringstream s;s<<"ASB_APEX6_LIVE_REVIEW_V1\n"<<live::scope<<'\n'<<contract<<'\n'<<r.prepared<<' '<<r.policy.seconds<<' '<<std::setprecision(17)<<r.policy.initialGain<<'\n'<<r.source<<'\n'<<r.exe<<'\n'<<r.libraryHash<<'\n'<<std::quoted(r.library)<<'\n'<<std::quoted(r.input)<<'\n'<<std::quoted(r.fixture)<<'\n'<<std::quoted(r.fixtureHash)<<'\n'<<std::quoted(r.isolationLease)<<'\n'<<std::quoted(r.isolationToken)<<'\n'<<ex::encodeGripBaseline(r.baseline);return s.str();}
Review decode(const std::string& text){
    std::istringstream in(text);Review r;std::string line;
    std::getline(in,line);require(line=="ASB_APEX6_LIVE_REVIEW_V1","wrong review schema");std::getline(in,line);require(line==live::scope,"wrong review scope");std::getline(in,line);require(line==contract,"changed live contract");
    in>>r.prepared>>r.policy.seconds>>r.policy.initialGain>>r.source>>r.exe>>r.libraryHash>>std::quoted(r.library)>>std::quoted(r.input)>>std::quoted(r.fixture)>>std::quoted(r.fixtureHash)>>std::quoted(r.isolationLease)>>std::quoted(r.isolationToken);require(bool(in),"truncated review");require(in.get()=='\n',"noncanonical review");
    std::string baseline((std::istreambuf_iterator<char>(in)),{});r.baseline=ex::decodeGripBaseline(baseline);r.policy.validate();ex::gripLifecyclePlan(r.baseline);require(encode(r)==text,"tampered/noncanonical review");return r;
}
void checkReview(const Review& r,bool fresh){require(r.source==ASB_APEX6_SOURCE_DIGEST&&r.exe==ex::sha256File(executable()),"source/executable hash changed");require(r.libraryHash==cap::sha256File(ex::wide(r.library)),"libVIIPER hash changed");require(r.fixture.empty()?r.fixtureHash.empty():r.fixtureHash==cap::sha256File(ex::wide(r.fixture)),"fixture hash changed");require(r.baseline.physicalOrigin,"physical live requires physical baselines");if(fresh)require(unixNow()>=r.prepared&&unixNow()-r.prepared<=300,"two-baseline review expired");}
ex::Binding select(const std::string& instance){auto devices=ex::vendorInterfaces();std::erase_if(devices,[&](const auto& d){return ex::utf8(d.instanceId)!=instance;});require(devices.size()==1,"exact vendor instance absent/ambiguous");auto b=ex::inspectInterface(devices.front());b.access=ex::AccessMode::Shared;return b;}
void checkInput(const Review& r){std::string error;auto devices=cap::captureInputDevices(error);require(error.empty(),"input enumeration failed");unsigned matches=0;for(const auto& d:devices)if(cap::utf8(d.instanceId)==r.input&&cap::utf8(d.containerId)==r.baseline.binding.container)++matches;require(matches==1,"input and vendor interfaces must match the same exact container");}
bool nativeEvidence(ex::WindowsTransport& io,const std::filesystem::path& out,const std::string& name){const bool resolved=io.finish();std::ostringstream s;for(const auto& t:io.timings())s<<"{\"time_us\":"<<t.observed.count()<<",\"event\":"<<ex::json(t.event)<<",\"operation\":"<<t.operation<<",\"transferred\":"<<t.transferred<<",\"error\":"<<t.error<<",\"deadline_us\":"<<t.deadline.count()<<",\"wait_ms\":"<<t.requestedWaitMs<<",\"wait_result\":"<<t.waitResult<<",\"raw\":"<<ex::json(ex::hex({t.raw.data(),t.rawSize}))<<"}\n";writeNew(out/name,s.str());return resolved&&io.timingComplete();}
void drainNative(ex::WindowsTransport& io,ex::Evidence& evidence){io.drainLiveTimings([&](const ex::NativeTiming& t){std::ostringstream s;s<<"{\"time_us\":"<<t.observed.count()<<",\"event\":"<<ex::json(t.event)<<",\"operation\":"<<t.operation<<",\"transferred\":"<<t.transferred<<",\"error\":"<<t.error<<",\"deadline_us\":"<<t.deadline.count()<<",\"wait_ms\":"<<t.requestedWaitMs<<",\"wait_result\":"<<t.waitResult<<",\"raw\":"<<ex::json(ex::hex({t.raw.data(),t.rawSize}))<<"}\n";evidence.recordLine(s.str());});}
void manifest(const std::filesystem::path& out){std::ostringstream s;s<<"{\"scope\":"<<ex::json(live::scope)<<",\"source_sha256\":"<<ex::json(ASB_APEX6_SOURCE_DIGEST)<<",\"operator_assessment_included\":false,\"files\":[";bool first=true;for(const auto& entry:std::filesystem::recursive_directory_iterator(out)){if(!entry.is_regular_file())continue;if(!first)s<<',';first=false;s<<"{\"name\":"<<ex::json(ex::utf8(std::filesystem::relative(entry.path(),out).wstring()))<<",\"sha256\":"<<ex::json(cap::sha256File(entry.path()))<<'}';}s<<"]}\n";writeNew(out/"manifest.json",s.str());}
int prepare(const Options& o){
    const auto lease=std::filesystem::absolute(get(o,L"--isolation-lease"));const auto token=ex::utf8(get(o,L"--isolation-token"));
    live::validateIsolationLease(read(lease),token,unixNow());
    const std::filesystem::path out=get(o,L"--output");ex::Evidence trace(out);
    auto binding=select(ex::utf8(get(o,L"--device")));ex::ExperimentLock lock(binding.container);
    ex::GripBaseline baseline;
    for(unsigned i=0;i<2;++i){auto io=ex::openGripBaselineTransport(binding);ex::Session session(*io,trace);auto b=ex::acquireGripBaseline(session);require(nativeEvidence(*io,out,"baseline-"+std::to_string(i)+"-timing.jsonl"),"baseline native finalization failed");writeNew(out/("baseline-"+std::to_string(i)+".asb"),ex::encodeGripBaseline(b));if(i)require(b==baseline,"fresh baselines differ");baseline=b;}
    Review r;r.baseline=baseline;r.isolationLease=ex::utf8(lease.wstring());r.isolationToken=token;r.policy.seconds=o.contains(L"--seconds")?integer(get(o,L"--seconds")):60;
    if(o.contains(L"--gain")){std::size_t used=0;auto value=get(o,L"--gain");r.policy.initialGain=std::stod(value,&used);require(used==value.size(),"invalid gain");}
    r.policy.validate();require(r.policy.initialGain*2==std::floor(r.policy.initialGain*2),"initial console gain must use 0.5 increments");r.prepared=unixNow();r.source=ASB_APEX6_SOURCE_DIGEST;r.exe=ex::sha256File(executable());r.library=ex::utf8(std::filesystem::absolute(get(o,L"--library")).wstring());r.libraryHash=cap::sha256File(ex::wide(r.library));r.input=ex::utf8(get(o,L"--input-device"));checkInput(r);
    if(o.contains(L"--fixture")){r.fixture=ex::utf8(std::filesystem::absolute(get(o,L"--fixture")).wstring());const auto pcm=cap::loadFixturePcm(ex::wide(r.fixture));require(pcm.size()<=384000*60,"fixture exceeds 60 seconds");r.fixtureHash=cap::sha256File(ex::wide(r.fixture));}
    writeNew(out/"review.asb",encode(r));require(trace.finish(),"prepare trace failed");manifest(out);return 0;
}
int rehearse(const Options& o){
    const std::filesystem::path out=get(o,L"--output");auto b=gripRehearsalFixture();live::Policy policy;std::string reviewHash="synthetic";
    if(o.contains(L"--manifest")){auto text=read(get(o,L"--manifest"));auto review=decode(text);checkReview(review,false);b=review.baseline;policy=review.policy;reviewHash=hash(text);}
    else if(o.contains(L"--seconds")){policy.seconds=integer(get(o,L"--seconds"));policy.validate();}
    ex::Evidence trace(out,{32*1024*1024,1024*1024,65536});
    ex::FakeIo io(b);live::Stream stream(policy.initialGain);live::RawQueue queue([&]{return io.now();});std::uint64_t sequence=0;Time previous{-1};
    live::Control control{[&](Time t){io.clock=t;},[&]{return queue.failed();},[&]{return policy.continuous()&&io.clock>=Time(300000000);},[&]{if(io.clock!=previous){++sequence;queue.submit(livePcm(sequence,1,sequence%3==0?3:2));while(auto item=queue.pop())stream.ingest(*item,io.clock);previous=io.clock;}},[&](Time t){return stream.packet(t);},[&]{while(queue.pop()){}stream.reset();},[](Time){}};
    auto result=live::run(io,trace,b,policy,control);if(!trace.finish()){result.complete=false;result.failure="rehearsal trace failed";}
    writeNew(out/"result.json",live::resultJson(result));if(result.complete)writeNew(out/"rehearsal.asb",std::string("ASB_APEX6_LIVE_REHEARSAL_V1\n")+reviewHash+"\n"+ASB_APEX6_SOURCE_DIGEST+"\n");manifest(out);return result.complete?0:1;
}
int approve(const Options& o){
    DWORD mode=0;require(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode),"approval requires interactive console; redirected consent refused");
    auto text=read(get(o,L"--manifest"));auto r=decode(text);checkReview(r,true);require(read(get(o,L"--rehearsal"))==std::string("ASB_APEX6_LIVE_REHEARSAL_V1\n")+hash(text)+"\n"+ASB_APEX6_SOURCE_DIGEST+"\n","matching successful rehearsal required");
    std::cout<<"Live DualSense PCM and HID rumble to Apex6 grips. Gain "<<r.policy.initialGain<<"; "<<(r.policy.continuous()?"continuous until Q":"timed session")<<". Review and automatic integrity checks saved with this session.\n";
    if(!live::enforceDispatchLateness)std::cout<<"DEBUG: dispatch lateness is recorded but not rejected. Minimum spacing and USB write deadlines remain enforced.\n";
    auto yes=[](const char* prompt){std::cout<<prompt<<"\nType YES: "<<std::flush;std::string answer;require(bool(std::getline(std::cin,answer))&&answer=="YES","operator declined");};
    yes("Confirm direct USB, receiver unplugged, controller writers closed, observed recovery from previous tests, normal input/vibration now, supervision throughout, and readiness to disconnect on unexpected behavior.");
    yes(r.policy.continuous()?"Accept continuous grip output until Q, gain 0..12 and provisional 0.75 ceiling. Fresh matching baselines/rehearsal do not prove physical recovery. Faults or Ctrl+C stop ALL physical traffic, including cleanup; disconnect on unexpected behavior.":"Confirm two fresh matching baselines and this exact rehearsal. Accept gain 0..12, provisional 0.75 ceiling (not calibrated motor force), maximum 60 seconds, shared access/reply attribution uncertainty, unacquired RAM 1/4/5, and unverified restore envelopes. Any fault or Ctrl+C stops ALL physical traffic including cleanup; matching RAM never proves recovery.");
    live::Approval a;auto& c=a.checkpoints;c.manifestHash=hash(text);c.confirmedUnixSeconds=unixNow();c.approved=c.observer=c.powerOffReady=c.normalVibration=c.knownWritersQuiesced=c.directUsb=c.failStopAccepted=c.backgroundRiskAccepted=c.restorationRiskAccepted=c.reducedAuditAccepted=c.powerCycled=c.restoreOnlyAccepted=a.liveAccepted=true;
    live::Authorization::approve(r.baseline,r.policy,a,hash(text),unixNow());writeNew(get(o,L"--output"),live::encodeApproval(a));return 0;
}
std::string metricsJson(const live::Stream& stream,const live::RawQueue& queue,std::uint64_t discarded){const auto& m=stream.metrics();auto q=queue.metrics();std::ostringstream s;s<<"{\"raw_records\":"<<q.records<<",\"raw_rejected\":"<<q.rejected<<",\"raw_discarded_for_output\":"<<discarded<<",\"raw_peak_bytes\":"<<q.peakBytes<<",\"pcm_peak_samples\":"<<m.peakSamples<<",\"stale_records\":"<<m.staleRecords<<",\"dropped_samples\":"<<m.droppedSamples<<",\"underrun_samples\":"<<m.underrunSamples<<",\"resets\":"<<m.resets<<",\"valid_pcm_records\":"<<m.validRecords<<",\"hid_accepted\":"<<m.hidAccepted<<",\"hid_ignored\":"<<m.hidIgnored<<",\"hid_suppressed\":"<<m.hidSuppressed<<",\"hid_timeouts\":"<<m.hidTimeouts<<",\"hid_samples\":"<<m.hidSamples<<",\"clipped_samples\":"<<m.strength.clippedSamples<<",\"peak_before_limit\":"<<m.strength.peakBeforeLimit<<",\"peak_after_limit\":"<<m.strength.peakAfterLimit<<"}\n";return s.str();}
int executeWorker(const Options& o,HANDLE cancel,Shared& shared){
    const std::filesystem::path out=get(o,L"--output");
    auto text=read(get(o,L"--manifest")),approvalText=read(get(o,L"--approval"));require(hash(text)==ex::utf8(get(o,L"--review-hash"))&&hash(approvalText)==ex::utf8(get(o,L"--approval-hash")),"supervisor-approved inputs changed");auto r=decode(text);checkReview(r,true);
    const auto evidenceBudget=r.policy.continuous()?std::numeric_limits<std::size_t>::max():16*1024*1024;
    ex::Evidence trace(out,{evidenceBudget,1024*1024,65536});
    std::unique_ptr<ex::Evidence> nativeTrace;if(r.policy.continuous())nativeTrace=std::make_unique<ex::Evidence>(out/"native",ex::TraceLimits{evidenceBudget,1024*1024,65536});
    auto isolationHealthy=[&]{const auto lease=ex::wide(r.isolationLease);require(!std::filesystem::exists(std::filesystem::path(lease).parent_path()/"isolation-failure.txt"),"physical isolation failed");live::validateIsolationLease(read(lease),r.isolationToken,unixNow());};
    isolationHealthy();
    auto authorization=live::Authorization::approve(r.baseline,r.policy,live::decodeApproval(approvalText),hash(text),unixNow());writeNew(out/"review.asb",text);writeNew(out/"approval.asb",approvalText);
    authorization.activate(unixNow());
    ex::ExperimentLock lock(r.baseline.binding.container);require(select(r.baseline.binding.instance)==r.baseline.binding,"physical binding changed");checkInput(r);
    live::RawQueue queue;live::Stream stream(r.policy.initialGain);cap::Limits limits;limits.seconds=r.policy.continuous()?0:240;limits.continuous=r.policy.continuous();limits.outputBytes=256*1024*1024;
    cap::FeedbackRecorder recorder(out/"raw",limits);std::string error;require(recorder.start(error),error.c_str());
    cap::WindowsCaptureInput input;cap::WindowsCaptureBackend backend(queue);cap::WindowsFixturePlayer player;asb::platform::VirtualDualSenseAudioEndpointProtection audio;
    Handle libraryPin,fixturePin;
    std::jthread inputThread;std::atomic_bool inputReady=false;std::unique_ptr<ex::WindowsTransport> io;live::Result result;bool audioVerified=false;
    auto cancelled=[&]{const auto status=WaitForSingleObject(cancel,0);return status!=WAIT_TIMEOUT||queue.failed()||recorder.failed()||!trace.healthy()||(nativeTrace&&!nativeTrace->healthy());};
    LONG revision=InterlockedCompareExchange(&shared.revision,0,0);
    std::uint64_t discarded=0;
    auto drain=[&](bool feed){bool valid=false;std::size_t charge=0;while(auto item=queue.pop()){recorder.submit(item->bytes,item->terminal);charge+=item->bytes.size()+128;if(feed)valid=stream.ingest(*item,live::monotonic())||valid;else ++discarded;require(charge<=4*1024*1024,"raw drain work budget");}return valid;};
    auto controls=[&]{auto next=InterlockedCompareExchange(&shared.revision,0,0);if(next!=revision){auto gain=InterlockedCompareExchange(&shared.gainHalf,0,0);require(gain>=0&&gain<=24,"invalid shared gain");stream.gain(gain*.5);trace.record(live::monotonic(),"gain_target_half",{},next,gain);revision=next;}};
    try {
        libraryPin.h=CreateFileW(ex::wide(r.library).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);require(libraryPin.h!=INVALID_HANDLE_VALUE,"cannot pin approved library against replacement");
        if(!r.fixture.empty()){fixturePin.h=CreateFileW(ex::wide(r.fixture).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);require(fixturePin.h!=INVALID_HANDLE_VALUE,"cannot pin approved fixture");}
        checkReview(r,true);
        require(input.open(ex::wide(r.input),error),error.c_str());require(audio.capture(error),error.c_str());
        require(backend.open(ex::wide(r.library),error),error.c_str());require(audio.protectAfterVirtualDualSenseStart(std::chrono::milliseconds(2000),error),error.c_str());
        inputThread=std::jthread([&](std::stop_token stop){try{asb::dualsense::DualSenseInputState state;std::string localError;auto leaseCheck=live::monotonic();while(!stop.stop_requested()&&!cancelled()){if(live::monotonic()>=leaseCheck){try{isolationHealthy();}catch(...){queue.fail("physical isolation lease lost");break;}leaseCheck=live::monotonic()+Time(250000);}if(!input.poll(state,localError)||!backend.update(state,localError)){queue.fail("physical input forwarding failed");break;}inputReady=true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}}catch(...){queue.fail("input forwarding exception");}});
        if(!r.fixture.empty())player.start(ex::wide(r.fixture),backend.serial(),recorder);
        const auto pcmDeadline=live::monotonic()+Time(120000000);bool valid=false;InterlockedExchange(&shared.ready,1);
        while(!(valid&&inputReady)){progress(shared);require(!cancelled(),"live cancelled before feedback");require(!InterlockedCompareExchange(&shared.orderly,0,0),"operator stopped before entry");require(r.policy.continuous()||live::monotonic()<pcmDeadline,"valid PCM/HID rumble wait expired");controls();valid=drain(true)||valid;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        require(!cancelled(),"live feedback failed before entry");authorization.check(unixNow());
        io=ex::openLiveTransport(authorization,unixNow,cancelled);ex::WindowsPulseWait wait(cancel,[&]{return io->now();});
        live::Control control{[&](Time due){wait.waitUntil(due);},cancelled,[&]{return InterlockedCompareExchange(&shared.orderly,0,0)!=0;},[&]{progress(shared);controls();drain(true);if(nativeTrace)drainNative(*io,*nativeTrace);},[&](Time t){return stream.packet(t);},[&]{drain(true);stream.discardPcm();},[&](Time t){io->stopLive(t);},[&]{progress(shared);}};
        result=live::run(*io,trace,r.baseline,r.policy,control,[&]{authorization.check(unixNow());});
    }catch(const std::exception& e){result.failure=e.what();result.stopReason="failure";}
    progress(shared);InterlockedExchange(&shared.finishing,1);
    // Stop and join the sole virtual input writer before neutralizing/detaching.
    if(inputThread.joinable()){inputThread.request_stop();inputThread.join();}
    if(!r.fixture.empty()){player.stop(result.complete);recorder.metadata("fixture_endpoint",player.endpoint());recorder.metadata("fixture_sha256",r.fixtureHash);recorder.metadata("fixture_error",player.error());}
    std::string cleanupError;if(!backend.serial().empty()&&!backend.update({},cleanupError)){result.complete=false;result.failure+="; virtual neutral failed";}
    if(audio.captured()&&!audio.protectAfterVirtualDualSenseStart(std::chrono::milliseconds(250),cleanupError)){result.complete=false;result.failure+="; audio protection failed";}
    if(!backend.close(cleanupError)){result.complete=false;result.failure+="; detach failed";}
    // close() is the callback quiescence barrier. Only now drain terminal events.
    try{drain(false);}catch(const std::exception& e){result.complete=false;result.failure+=e.what();}
    if(audio.captured()){asb::platform::VirtualDualSenseAudioEndpointProtection after;if(after.capture(cleanupError)){audioVerified=audio.defaultEndpointIds()==after.defaultEndpointIds();std::ostringstream s;s<<"{\"verified\":"<<(audioVerified?"true":"false")<<",\"before\":[";for(unsigned i=0;i<3;++i){if(i)s<<',';s<<ex::json(ex::utf8(audio.defaultEndpointIds()[i]));}s<<"],\"after\":[";for(unsigned i=0;i<3;++i){if(i)s<<',';s<<ex::json(ex::utf8(after.defaultEndpointIds()[i]));}s<<"]}\n";writeNew(out/"audio-defaults.json",s.str());}}
    if(!audioVerified){result.complete=false;result.failure+="; audio defaults not verified";}
    if(nativeTrace){try{if(io){const bool resolved=io->finish();drainNative(*io,*nativeTrace);require(resolved&&io->timingComplete(),"native finalization failed");}require(nativeTrace->finish(),"native trace failed");}catch(const std::exception& e){result.complete=false;result.failure+=std::string("; ")+e.what();}}
    else if(io&&!nativeEvidence(*io,out,"native-timing.jsonl")){result.complete=false;result.failure+="; native finalization failed";}
    recorder.metadata("scope",live::scope);if(!recorder.finish(input.description(),r.libraryHash,error)){result.complete=false;result.failure+="; raw evidence incomplete: "+error;}
    if(queue.failed()){result.complete=false;result.failure+=std::string("; ")+queue.failure();}
    if(!trace.finish()){result.complete=false;result.failure+="; trace incomplete";}
    writeNew(out/"metrics.json",metricsJson(stream,queue,discarded));writeNew(out/"result.json",live::resultJson(result));manifest(out);return result.complete?2:1;
}
std::wstring quote(const std::wstring& text){std::wstring out=L"\"";unsigned slashes=0;for(auto c:text){if(c==L'\\'){++slashes;continue;}if(c==L'\"')out.append(slashes*2+1,L'\\');else out.append(slashes,L'\\');slashes=0;out+=c;}out.append(slashes*2,L'\\');return out+L'\"';}
int supervise(const std::wstring& command,Options o){
    const bool executing=command==L"execute";const auto out=std::filesystem::absolute(get(o,L"--output"));
    bool continuous=false;double initial=1;if(executing){auto text=read(get(o,L"--manifest")),a=read(get(o,L"--approval"));auto r=decode(text);checkReview(r,true);live::Authorization::approve(r.baseline,r.policy,live::decodeApproval(a),hash(text),unixNow());continuous=r.policy.continuous();initial=r.policy.initialGain;writeNew(std::filesystem::path(get(o,L"--approval")+L".used"),hash(text)+"\n");o[L"--review-hash"]=ex::wide(hash(text));o[L"--approval-hash"]=ex::wide(hash(a));}
    require(std::filesystem::create_directory(out),"output directory must be new");
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle job,parent,cancel,mapping;job.h=CreateJobObjectW(nullptr,nullptr);require(job.h,"job creation failed");JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;require(SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"job limits failed");
    require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&parent.h,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0),"supervisor handle failed");cancel.h=CreateEventW(&sa,TRUE,FALSE,nullptr);mapping.h=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,sizeof(Shared),nullptr);require(cancel.h&&mapping.h,"control handles failed");Mapping view;view.data=static_cast<Shared*>(MapViewOfFile(mapping.h,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));require(view.data,"control mapping failed");InterlockedExchange(&view.data->gainHalf,static_cast<LONG>(initial*2));
    progress(*view.data);InterlockedExchange64(&view.data->supervisorProgress,static_cast<LONG64>(GetTickCount64()));
    o[L"--output"]=(out/"worker").wstring();o[L"--parent"]=std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.h));o[L"--cancel"]=std::to_wstring(reinterpret_cast<std::uintptr_t>(cancel.h));o[L"--mapping"]=std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping.h));
    auto exe=executable();auto cmd=quote(exe.wstring())+L" _worker-"+command;for(const auto& [key,value]:o)cmd+=L" "+key+L" "+quote(value);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};require(CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&process),"worker launch failed");Handle child,thread;child.h=process.hProcess;thread.h=process.hThread;
    if(!AssignProcessToJobObject(job.h,child.h)||ResumeThread(thread.h)==DWORD(-1)){TerminateProcess(child.h,1);throw ProtocolError("worker supervision failed");}
    cancelHandle=cancel.h;require(SetConsoleCtrlHandler(consoleHandler,TRUE),"console cancel handler failed");std::ofstream changes(out/"console-controls.jsonl");require(bool(changes),"console trace open failed");
    if(executing)std::cout<<(continuous?"DualSense ready for sources; continuous grip transfer. Q stops. + / - adjusts strength.\n":"Waiting for DualSense PCM or HID rumble (up to 120 s). Use +, -, 0, 1, Q. Ctrl+C stops all physical traffic immediately.\n");
    const auto deadline=live::monotonic()+Time(245000000);bool timeout=false,controlsFailed=false;
    while(WaitForSingleObject(child.h,20)==WAIT_TIMEOUT){InterlockedExchange64(&view.data->supervisorProgress,static_cast<LONG64>(GetTickCount64()));if(continuous?stalled(view.data->workerProgress,InterlockedCompareExchange(&view.data->finishing,0,0)?245000:15000):live::monotonic()>=deadline){timeout=true;SetEvent(cancel.h);TerminateProcess(child.h,1);break;}if(executing&&_kbhit()){int key=_getch();LONG gain=InterlockedCompareExchange(&view.data->gainHalf,0,0);bool changed=true;if(key=='+')gain=std::min<LONG>(24,gain+1);else if(key=='-')gain=std::max<LONG>(0,gain-1);else if(key=='0')gain=0;else if(key=='1')gain=2;else if(key=='q'||key=='Q'){InterlockedExchange(&view.data->orderly,1);changed=false;}else changed=false;if(changed){InterlockedExchange(&view.data->gainHalf,gain);InterlockedIncrement(&view.data->revision);std::cout<<"Gain target "<<gain*.5<<'\n';}changes<<"{\"time_us\":"<<live::monotonic().count()<<",\"key\":"<<key<<",\"gain_half\":"<<gain<<"}\n"<<std::flush;if(!changes){controlsFailed=true;SetEvent(cancel.h);}}}
    SetConsoleCtrlHandler(consoleHandler,FALSE);cancelHandle=nullptr;changes.close();DWORD code=1;const bool exited=WaitForSingleObject(child.h,1000)==WAIT_OBJECT_0;if(exited)GetExitCodeProcess(child.h,&code);
    writeNew(out/"supervisor.json",std::string("{\"worker_exited\":")+(exited?"true":"false")+",\"deadline_exceeded\":"+(timeout?"true":"false")+",\"control_evidence_failed\":"+(controlsFailed?"true":"false")+",\"exit_code\":"+std::to_string(code)+",\"recovery_inferred\":false}\n");
    if(timeout||controlsFailed||!exited)return 1;
    if(executing)std::cout<<(code==2?"Bridge stopped.\n":"Bridge stopped on failure; see saved evidence.\n");return code==0?0:code==2?2:1;
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc<2||std::wstring(argv[1])==L"--help"){std::cout<<"ApexSenseBridgeApex6LiveBridge prepare --device ID --input-device ID --library DLL --isolation-lease PATH --isolation-token TOKEN --output NEW_DIR [--seconds 60] [--gain 1]\nrehearse --output NEW_DIR [--manifest REVIEW]\napprove --manifest REVIEW --rehearsal FILE --output NEW_APPROVAL\nexecute --manifest REVIEW --approval FILE --output NEW_DIR\nExperimental grip-only scope "<<live::scope<<". No drivers, game launch or trigger output.\n";return 0;}
    const std::wstring command=argv[1];Options o;for(int i=2;i<argc;i+=2){require(i+1<argc&&std::wstring(argv[i]).starts_with(L"--"),"expected option/value pairs");require(o.emplace(argv[i],argv[i+1]).second,"duplicate option");}
    if(command==L"check-input"){only(o,{L"--input-device"});cap::WindowsCaptureInput input;std::string error;if(!input.open(get(o,L"--input-device"),error))throw ProtocolError(error);asb::dualsense::DualSenseInputState state{};if(!input.poll(state,error))throw ProtocolError(error);std::cout<<input.description()<<'\n';return 0;}
    if(command==L"check-vendor-access"){only(o,{L"--device"});auto binding=select(ex::utf8(get(o,L"--device")));binding.access=ex::AccessMode::Exclusive;auto io=ex::openQueryTransport(binding);require(io->finish(),"exclusive vendor probe did not close cleanly");std::cout<<"exclusive vendor open succeeded; no commands sent\n";return 0;}
    if(command.starts_with(L"_worker-")){
        auto handle=[&](const wchar_t* key){auto s=get(o,key);std::size_t used=0;auto n=std::stoull(s,&used);require(used==s.size()&&n,"invalid inherited handle");return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(n));};
        auto parent=handle(L"--parent"),cancel=handle(L"--cancel"),mapping=handle(L"--mapping");BOOL job=FALSE;require(GetProcessId(parent)&&GetProcessId(parent)!=GetCurrentProcessId()&&WaitForSingleObject(parent,0)==WAIT_TIMEOUT&&IsProcessInJob(GetCurrentProcess(),nullptr,&job)&&job,"worker requires live supervisor/job");require(WaitForSingleObject(cancel,0)==WAIT_TIMEOUT,"worker cancelled");Mapping view;view.data=static_cast<Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));require(view.data,"worker controls missing");
        const bool continuous=command==L"_worker-execute"&&decode(read(get(o,L"--manifest"))).policy.continuous();
        progress(*view.data);
        std::jthread watchdog([&](std::stop_token stop){const auto end=live::monotonic()+Time(240000000);while(!stop.stop_requested()){const bool expired=continuous?(stalled(view.data->workerProgress,InterlockedCompareExchange(&view.data->finishing,0,0)?240000:10000)||stalled(view.data->supervisorProgress,10000)):live::monotonic()>=end;if(expired||WaitForSingleObject(parent,0)!=WAIT_TIMEOUT){SetEvent(cancel);ExitProcess(1);}std::this_thread::sleep_for(std::chrono::milliseconds(20));}});
        if(command==L"_worker-prepare")return prepare(o);if(command==L"_worker-execute")return executeWorker(o,cancel,*view.data);throw ProtocolError("unknown worker");
    }
    if(command==L"prepare"){only(o,{L"--device",L"--input-device",L"--library",L"--output",L"--seconds",L"--gain",L"--fixture",L"--isolation-lease",L"--isolation-token"});return supervise(command,o);}
    if(command==L"rehearse"){only(o,{L"--output",L"--manifest",L"--seconds"});return rehearse(o);}
    if(command==L"approve"){only(o,{L"--output",L"--manifest",L"--rehearsal"});return approve(o);}
    if(command==L"execute"){only(o,{L"--output",L"--manifest",L"--approval"});return supervise(command,o);}
    throw ProtocolError("unknown live command");
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
