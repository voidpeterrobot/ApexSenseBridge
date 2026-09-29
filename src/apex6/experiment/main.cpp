#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "apex6/experiment/Session.h"
#include "apex6/experiment/Evidence.h"
#include "apex6/experiment/WindowsIo.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6/experiment/ApprovalConsole.h"
#include "apex6/experiment/PulseEvidence.h"
#include "apex6/experiment/WindowsPulseWait.h"
#include "BuildIdentity.h"
#include "apex6_rehearsal_fixture.h"
#include <iostream>
#include <map>
#include <sstream>

using namespace asb::apex6;
using namespace asb::apex6::experiment;
namespace {
struct Handle {HANDLE h=nullptr;~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
std::atomic<HANDLE> pulseCancelEvent=nullptr;
BOOL WINAPI pulseConsoleHandler(DWORD event) {
    if(event!=CTRL_C_EVENT&&event!=CTRL_BREAK_EVENT&&event!=CTRL_CLOSE_EVENT)return FALSE;
    const auto h=pulseCancelEvent.load();if(!h)return FALSE;SetEvent(h);return TRUE;
}
struct PulseCancelConsole {
    Handle event;bool installed=false;
    explicit PulseCancelConsole(bool enabled) {
        if(!enabled)return;SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};event.h=CreateEventW(&sa,TRUE,FALSE,nullptr);
        if(!event.h)throw ProtocolError("pulse cancel event creation failed");
        pulseCancelEvent=event.h;
        if(!SetConsoleCtrlHandler(pulseConsoleHandler,TRUE)){pulseCancelEvent=nullptr;throw ProtocolError("pulse cancel handler failed");}installed=true;
    }
    ~PulseCancelConsole(){if(installed){SetConsoleCtrlHandler(pulseConsoleHandler,FALSE);pulseCancelEvent=nullptr;}}
};
std::filesystem::path executable() {
    std::wstring path(32768,L'\0');auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!n||n==path.size())throw ProtocolError("cannot resolve executable");path.resize(n);return path;
}
std::string hashText(const std::string& text){return sha256({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
std::string readText(const std::filesystem::path& path) {
    if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>65536)throw ProtocolError("expected regular input <=64 KiB");
    std::ifstream in(path,std::ios::binary);std::string text((std::istreambuf_iterator<char>(in)),{});if(in.bad())throw ProtocolError("input read failed");return text;
}
void writeNew(const std::filesystem::path& path,const std::string& text) {
    Handle file;file.h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file.h==INVALID_HANDLE_VALUE)throw ProtocolError("output must be a new writable file");
    DWORD written=0;if(!WriteFile(file.h,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)||written!=text.size()||!FlushFileBuffers(file.h))throw ProtocolError("evidence output failed");
}
void finalize(const std::filesystem::path& out,bool complete,const std::string& error,bool physical,bool uncertain,AccessMode access=AccessMode::Synthetic,const std::string& lock="not_required",unsigned queries=0,unsigned actuators=0,bool inputOnly=false,const std::string& scope="unspecified") {
    std::ostringstream summary;summary<<"{\"schema\":\"asb.apex6.result.v2\",\"complete\":"<<(complete?"true":"false")<<",\"physical_queries\":"<<(physical&&queries?"true":"false")
        <<",\"query_write_attempts\":"<<queries<<",\"actuator_write_attempts\":"<<actuators
        <<",\"report_io_kind\":"<<json(physical?"physical":(queries||actuators)?"simulated":"none")
        <<",\"access_mode\":"<<json(accessName(access))<<",\"cooperative_lock\":"<<json(lock)
        <<",\"input_only\":"<<(inputOnly?"true":"false")
        <<",\"evidence_scope\":"<<json(scope)<<",\"full_configuration_preservation_proven\":false"
        <<",\"requested_access\":"<<(access==AccessMode::Exclusive||access==AccessMode::Shared?std::to_string(inputOnly?GENERIC_READ:GENERIC_READ|GENERIC_WRITE):"null")
        <<",\"requested_share_flags\":"<<(access==AccessMode::Exclusive||access==AccessMode::Shared?std::to_string(queryShareFlags(access)):"null")
        <<",\"exclusive_ownership_proven\":false,\"other_writers_absent_proven\":false,\"response_attribution_proven\":false"
        <<",\"physical_actuation_attempted\":"<<(physical&&actuators?"true":"false")<<",\"device_state_uncertain\":"<<(uncertain?"true":"false")<<",\"normal_vibration_verified\":false,\"error\":"<<json(error)<<"}\n";
    writeNew(out/"summary.json",summary.str());
    std::ostringstream manifest;manifest<<"{\"schema\":\"asb.apex6.evidence.v2\",\"complete\":"<<(complete?"true":"false")<<",\"access_mode\":"<<json(accessName(access))<<",\"source_sha256\":"<<json(ASB_APEX6_SOURCE_DIGEST)<<",\"executable_sha256\":"<<json(sha256File(executable()))<<",\"files\":[";
    bool first=true;for(const auto* file:{L"trace.jsonl",L"io-timing.json",L"grip-lifecycle.json",L"grip-pulse.json",L"restore-observation.json",L"listen.json",L"ram5-diagnostic.json",L"snapshot.asb",L"grip-baseline.asb",L"access-probe.json",L"review.json",L"approval.asb",L"summary.json"}) {auto path=out/file;if(!std::filesystem::exists(path))continue;if(!first)manifest<<',';first=false;manifest<<"{\"name\":"<<json(utf8(file))<<",\"bytes\":"<<std::filesystem::file_size(path)<<",\"sha256\":"<<json(sha256File(path))<<'}';}
    manifest<<"]}\n";writeNew(out/"manifest.json",manifest.str());
}
bool lifecycleComplete(const GripLifecycleResult& r) {return r.sequenceComplete&&r.postflightMatches&&r.failure.empty();}
void writeLifecycle(const std::filesystem::path& out,const GripLifecycleResult& r) {
    std::ostringstream s;s<<"{\"schema\":\"asb.apex6.grip-lifecycle.v1\",\"sequence_complete\":"<<(r.sequenceComplete?"true":"false")
        <<",\"restore_replies\":["<<json(modeReplyName(r.restoreReplies[0]))<<','<<json(modeReplyName(r.restoreReplies[1]))<<']'
        <<",\"postflight_matches\":"<<(r.postflightMatches?"true":"false")<<",\"restoration_verified\":false,\"device_state_uncertain\":"<<(r.deviceStateUncertain?"true":"false")
        <<",\"failure\":"<<json(r.failure)<<"}\n";writeNew(out/"grip-lifecycle.json",s.str());
}
void writeObservation(const std::filesystem::path& out,const RestoreObservation& r) {
    std::ostringstream report;report<<"{\"schema\":\"asb.apex6.restore-observation.v1\",\"started\":"<<(r.started?"true":"false")
        <<",\"capture_complete\":"<<(r.complete?"true":"false")<<",\"additional_reports\":"<<r.reports
        <<",\"elapsed_us\":"<<r.elapsed.count()<<",\"max_ms\":500,\"max_reports\":32,\"first_reply\":"<<json(r.firstReply)
        <<",\"stop\":"<<json(r.stop)<<",\"error\":"<<json(r.error)<<",\"restoration_proven\":false,\"late_ack_authorizes_writes\":false}\n";
    writeNew(out/"restore-observation.json",report.str());
}
bool finishNative(WindowsTransport& io,const std::filesystem::path& out) {
    const auto resolved=io.finish();std::ostringstream text;
    text<<"{\"schema\":\"asb.apex6.host-io-timing.v2\",\"clock\":\"steady_clock_microseconds\",\"usb_arrival_timestamps\":false,\"completion_observed_not_arrival\":true,\"pending_io_resolved\":"<<(resolved?"true":"false")
        <<",\"timing_complete\":"<<(io.timingComplete()?"true":"false")<<",\"events\":[";
    bool first=true;for(const auto& t:io.timings()) {if(!first)text<<',';first=false;text<<"{\"observed_us\":"<<t.observed.count()<<",\"event\":"<<json(t.event)<<",\"native_operation\":"<<t.operation<<",\"transferred\":"<<t.transferred<<",\"win32_error\":"<<t.error<<",\"raw_hex\":"<<json(hex({t.raw.data(),t.rawSize}));
        if(t.waitMetadata)text<<",\"deadline_us\":"<<t.deadline.count()<<",\"requested_wait_ms\":"<<t.requestedWaitMs<<",\"wait_result\":"<<t.waitResult;
        text<<'}';}
    text<<"]}\n";writeNew(out/"io-timing.json",text.str());return resolved&&io.timingComplete();
}
std::string reviewManifest(const Snapshot& baseline) {
    const auto encoded=encodeSnapshot(baseline);std::ostringstream out;
    out<<"{\"schema\":\"asb.apex6.neutral-review.v2\",\"status\":"
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        <<json("NEUTRAL_ONLY_REQUIRES_FRESH_APPROVAL")
#else
        <<json("REVIEW_ONLY_PHYSICAL_EXECUTION_LOCKED")
#endif
        <<",\"access_mode\":"<<json(accessName(baseline.binding.access))<<",\"snapshot_schema\":"<<baseline.schema<<",\"source_sha256\":"<<json(ASB_APEX6_SOURCE_DIGEST)
        <<",\"executable_sha256\":"<<json(sha256File(executable()))<<",\"baseline_sha256\":"<<json(hashText(encoded))
        <<",\"baseline\":"<<json(encoded)<<",\"identity\":{\"device_type\":"<<unsigned(baseline.info.deviceType)<<",\"uid\":"<<json(hex(baseline.unit))<<",\"connection_field\":"<<unsigned(baseline.info.connection)<<",\"firmware\":[";
    for(std::size_t i=0;i<baseline.info.firmware.size();++i){if(i)out<<',';out<<baseline.info.firmware[i];}
    out<<"],\"format_versions\":[";
    for(std::size_t i=0;i<baseline.formats.size();++i){if(i)out<<',';out<<baseline.formats[i];}
    out<<"],\"active_slot\":"<<unsigned(baseline.config.slot)<<"},\"binding\":{\"instance\":"<<json(baseline.binding.instance)<<",\"container\":"<<json(baseline.binding.container)
        <<",\"input_report_id\":"<<unsigned(baseline.binding.layout.inputId)<<",\"output_report_id\":"<<unsigned(baseline.binding.layout.outputId)
        <<",\"input_length_including_id\":"<<baseline.binding.layout.inputLength<<",\"output_length_including_id\":"<<baseline.binding.layout.outputLength<<"},\"grip_restore\":[";
    bool firstRestore=true;for(const auto& r:gripRestore(baseline.blocks[3])){if(!firstRestore)out<<',';firstRestore=false;out<<"{\"target\":"<<unsigned(r.target)<<",\"mode\":"<<unsigned(r.mode)<<",\"parameters_hex\":"<<json(hex(r.parameters))<<'}';}
    out<<"],\"transport\":\"operator-confirmed-direct-usb-only\",\"failure_policy\":\"no further traffic; operator power-off required after actuator uncertainty\""
        <<",\"limits\":{\"preflight_seconds\":30,\"preflight_attempts\":128,\"active_seconds\":15,\"active_attempts\":32,\"postflight_seconds\":30,\"postflight_attempts\":128,\"cleanup_by_seconds\":10,\"exchange_ms\":600,\"drain_reports\":64,\"mode_requests\":4,\"waveform_requests\":3,\"retries\":0}"
        <<",\"uncertainties\":[\"all-disabled frame not vendor-validated\",\"historical restore errors unresolved\",\"no protocol transaction IDs\",\"process exit does not prove stopped motors\",\"cooperative lock does not exclude other applications\",\"background ownership and reply attribution are unresolved\"],\"phases\":{";
    unsigned phase=0;for(const auto& plan:{snapshotPlan(baseline),neutralPlan(baseline),snapshotPlan(baseline)}) {
        if(phase)out<<',';out<<json(phase==0?"preflight":phase==1?"active":"postflight")<<":[";
        bool first=true;for(const auto& r:plan) {if(!first)out<<',';first=false;auto f=frame(r.command,r.payload);auto wire=baseline.binding.layout.wrap(f);out<<"{\"command\":"<<unsigned(r.command)<<",\"payload_hex\":"<<json(hex(r.payload))<<",\"windows_report_hex\":"<<json(hex(wire))<<'}';}
        out<<']';++phase;
    }
    out<<"}}\n";auto text=out.str();if(text.size()>65536)throw ProtocolError("review manifest exceeds 64 KiB");return text;
}
std::int64_t unixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string gripReviewManifest(const GripBaseline& baseline,bool restoreOnly=false,bool observe=false) {
    const auto encoded=encodeGripBaseline(baseline);std::ostringstream out;
    out<<"{\"schema\":"<<json(observe?"asb.apex6.grip-observe-review.v1":restoreOnly?"asb.apex6.grip-restore-review.v1":"asb.apex6.grip-neutral-review.v2")
        <<",\"scope\":"<<json(observe?"grip-left-restore-observe":restoreOnly?"grip-left-restore-only":"grip-only")<<",\"status\":"
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        <<json(observe?"RESTORE_OBSERVATION_REQUIRES_FRESH_APPROVAL":restoreOnly?"RESTORE_LEFT_ONLY_REQUIRES_FRESH_APPROVAL":"NEUTRAL_ONLY_REQUIRES_FRESH_GRIP_APPROVAL")
#else
        <<json("REHEARSAL_ONLY_PHYSICAL_EXECUTION_LOCKED")
#endif
        <<",\"source_sha256\":"<<json(ASB_APEX6_SOURCE_DIGEST)<<",\"executable_sha256\":"<<json(sha256File(executable()))
        <<",\"baseline_sha256\":"<<json(hashText(encoded))<<",\"baseline\":"<<json(encoded)
        <<",\"origin\":"<<json(baseline.physicalOrigin?"physical":"synthetic")<<",\"access_mode\":"<<json(accessName(baseline.binding.access))
        <<",\"unacquired_ram_ids\":[1,4,5],\"full_configuration_preservation_proven\":false"
        <<",\"failure_policy\":"<<json(observe?"after the single mode write forbid all further writes; only after a timely complete first reply permit bounded raw listening even on parse failure; no postflight or cleanup; late ACK never establishes recovery; power off afterward":"stop all traffic on unexpected failure; operator power-off may be required; no automatic cleanup retry")
        <<(observe?",\"limits\":{\"preflight_seconds\":30,\"preflight_attempts\":14,\"active_seconds\":2,\"active_attempts\":1,\"postflight_attempts\":0,\"exchange_ms\":600,\"observe_ms\":500,\"observe_reports\":32,\"drain_reports\":64,\"mode_requests\":1,\"waveform_requests\":0,\"retries\":0}":restoreOnly?",\"limits\":{\"preflight_seconds\":30,\"preflight_attempts\":14,\"active_seconds\":2,\"active_attempts\":1,\"postflight_seconds\":30,\"postflight_attempts\":14,\"exchange_ms\":600,\"drain_reports\":64,\"mode_requests\":1,\"waveform_requests\":0,\"retries\":0}":",\"limits\":{\"preflight_seconds\":30,\"preflight_attempts\":14,\"active_seconds\":15,\"active_attempts\":21,\"postflight_seconds\":30,\"postflight_attempts\":14,\"cleanup_by_seconds\":10,\"exchange_ms\":600,\"drain_reports\":64,\"mode_requests\":4,\"waveform_requests\":3,\"retries\":0}")
        <<",\"uncertainties\":[\"RAM 1/4/5 not acquired; stable CRC16 values do not prove full preservation\","
        <<json(restoreOnly?"left restore returned zero-count error 1 after streaming and after power cycle; meaning unresolved":"all-disabled frame not vendor-validated")
        <<",\"historical restore errors unresolved\",\"shared access does not establish sole ownership or response attribution\",\"process exit does not prove stopped motors\"]"
        <<",\"phases\":{";
    unsigned phase=0;for(const auto& plan:{gripBaselinePlan(),restoreOnly?gripRestoreLeftPlan(baseline):neutralPlan(baseline),observe?std::vector<Request>{}:gripBaselinePlan()}) {
        if(phase)out<<',';out<<json(phase==0?"preflight":phase==1?"active":"postflight")<<":[";
        bool first=true;for(const auto& r:plan){if(!first)out<<',';first=false;out<<"{\"command\":"<<unsigned(r.command)<<",\"payload_hex\":"<<json(hex(r.payload))<<",\"windows_report_hex\":"<<json(hex(baseline.binding.layout.wrap(frame(r.command,r.payload))))<<'}';}
        out<<']';++phase;
    }
    out<<"}}\n";const auto text=out.str();if(text.size()>65536)throw ProtocolError("grip review too large");return text;
}
std::string lifecycleReviewManifest(const GripBaseline& baseline) {
    const auto active=gripLifecyclePlan(baseline);const auto encoded=encodeGripBaseline(baseline);std::ostringstream out;
    out<<"{\"schema\":\"asb.apex6.grip-lifecycle-review.v1\",\"scope\":"<<json(gripLifecycleScope)
        <<",\"source_sha256\":"<<json(ASB_APEX6_SOURCE_DIGEST)<<",\"executable_sha256\":"<<json(sha256File(executable()))
        <<",\"baseline_sha256\":"<<json(hashText(encoded))<<",\"baseline\":"<<json(encoded)
        <<",\"reply_policy\":\"entry/exit require normal success ACK; individual restores admit normal success ACK or exact 32-byte captured zero-count/value-1 envelope as unverified evidence; all other outcomes fatal\""
        <<",\"capture_sha256\":\"a701b3fe7cee7d29301e19660dd4efe0b7d19dbf80dbcd183ea092d72a8a1a45\""
        <<",\"recovery_after_official_capture\":\"unverified; operator must check before another physical experiment\""
        <<",\"restoration_verified\":false,\"limits\":{\"query_writes\":28,\"actuator_writes\":4,\"waveform_writes\":0,\"exchange_ms\":600,\"active_seconds\":5,\"session_seconds\":75,\"retries\":0,\"dwell_ms\":0}"
        <<",\"failure_policy\":\"latched fail-stop; no cleanup; unknown/malformed/nonzero status/missing/stale/late replies, device loss and I/O or trace faults fatal\""
        <<",\"unacquired_ram_ids\":[1,4,5],\"physical_recovery_inferred_from_ram\":false,\"official_pipeline_reply_attribution\":false,\"phases\":{";
    unsigned phase=0;for(const auto& plan:{gripBaselinePlan(),active,gripBaselinePlan()}) {
        if(phase)out<<',';out<<json(phase==0?"preflight":phase==1?"active":"postflight")<<":[";bool first=true;
        for(const auto& r:plan){if(!first)out<<',';first=false;out<<"{\"command\":"<<unsigned(r.command)<<",\"payload_hex\":"<<json(hex(r.payload))<<",\"windows_report_hex\":"<<json(hex(baseline.binding.layout.wrap(frame(r.command,r.payload))))<<'}';}
        out<<']';++phase;
    }
    out<<"}}\n";return out.str();
}
// This is deliberately not a permissive JSON reader. Extract the one canonical
// snapshot string, then require byte-for-byte regeneration of the whole review.
std::string reviewBaselineText(const std::string& text) {
    const std::string marker="\"baseline\":\"";auto pos=text.find(marker);
    if(pos==std::string::npos)throw ProtocolError("review baseline missing");pos+=marker.size();std::string encoded;
    bool closed=false;
    while(pos<text.size()) {
        char c=text[pos++];if(c=='"'){closed=true;break;}
        // json() canonically writes every control byte as \\u00XX, including LF.
        // Baseline text is ASCII hex/numbers/spaces plus LF, so this is the only
        // escape permitted. Whole-review regeneration below rejects alternatives.
        if(c=='\\'){if(text.compare(pos,5,"u000a")!=0)throw ProtocolError("noncanonical baseline escape");pos+=5;c='\n';}
        else if(static_cast<unsigned char>(c)<32)throw ProtocolError("unescaped review control byte");
        encoded+=c;
    }
    if(!closed)throw ProtocolError("unterminated baseline");return encoded;
}
Snapshot checkedReview(const std::string& text) {
    auto s=decodeSnapshot(reviewBaselineText(text));
    if(reviewManifest(s)!=text)throw ProtocolError("manifest/baseline/executable/source/access mismatch");return s;
}
GripBaseline checkedGripReview(const std::string& text,bool restoreOnly=false,bool observe=false) {
    auto s=decodeGripBaseline(reviewBaselineText(text));
    if(gripReviewManifest(s,restoreOnly,observe)!=text)throw ProtocolError("grip manifest/baseline/executable/source/access mismatch");return s;
}
GripBaseline checkedLifecycleReview(const std::string& text) {
    auto s=decodeGripBaseline(reviewBaselineText(text));
    if(lifecycleReviewManifest(s)!=text)throw ProtocolError("lifecycle review/baseline/source/executable mismatch");return s;
}
std::string pulseReview(const GripBaseline& b) {return pulseReviewManifest(b,ASB_APEX6_SOURCE_DIGEST,sha256File(executable()));}
GripBaseline checkedPulseReview(const std::string& text) {
    auto b=decodeGripBaseline(reviewBaselineText(text));if(pulseReview(b)!=text)throw ProtocolError("pulse review/baseline/source/executable/fixture mismatch");return b;
}
int pulseRehearsal(const GripBaseline& b,const std::string& review,const std::filesystem::path& out,HANDLE cancellation=nullptr) {
    Evidence evidence(out);FakeIo io(b);Session session(io,evidence);
    io.rawResponseHook=[](auto& fake,auto& wire){if(fake.writes==28||fake.writes==29)wire=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");};
    PulseControl control{[&](Time due){io.clock=due;},[&]{return cancellation&&WaitForSingleObject(cancellation,0)!=WAIT_TIMEOUT;}};
    auto result=rehearseGripPulse(session,b,control);if(!evidence.finish())result.failure="trace incomplete";
    writeNew(out/"grip-pulse.json",pulseResultJson(result));writeNew(out/"review.json",review);
    finalize(out,result.complete(),result.failure,false,result.deviceStateUncertain,AccessMode::Synthetic,"not_required",session.queryAttempts(),session.actuatorAttempts(),false,gripPulseScope);
    std::cout<<(result.complete()?"GRIP PULSE DIAGNOSTIC COMPLETE (exit 2): operator qualification pending; 43 simulated writes; zero device opens\n":"Grip pulse rehearsal FAILED\n");return result.complete()?2:1;
}
#ifdef ASB_APEX6_NEUTRAL_RUNNER
int gripPulseWorker(const std::filesystem::path& reviewPath,const std::filesystem::path& approvalPath,const std::filesystem::path& out,const std::string& reviewHash,const std::string& approvalHash,HANDLE cancellation) {
    Evidence evidence(out);GripPulseResult result;std::string lockStatus="not_attempted";
    std::unique_ptr<ExperimentLock> lock;std::unique_ptr<WindowsTransport> io;std::unique_ptr<Session> session;
    try {
        const auto review=readText(reviewPath),approvalText=readText(approvalPath);
        if(hashText(review)!=reviewHash||hashText(approvalText)!=approvalHash)throw ProtocolError("pulse worker inputs changed");
        const auto b=checkedPulseReview(review);const auto auth=GripPulseAuthorization::approve(b,decodeGripPulseApproval(approvalText),hashText(review),unixSeconds());
        writeNew(out/"review.json",review);writeNew(out/"approval.asb",approvalText);
        // Timer and cancellation are checked before opening the physical device.
        auto wait=std::make_shared<WindowsPulseWait>(cancellation,[]{return std::chrono::duration_cast<Time>(std::chrono::steady_clock::now().time_since_epoch());});
        if(wait->cancelled()){result.cancelled=true;throw ProtocolError("pulse cancelled before open");}
        std::vector<asb::HidDeviceInfo> selected;for(const auto& d:vendorInterfaces())if(utf8(d.instanceId)==b.binding.instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("approved exact vendor instance absent or ambiguous");
        auto current=inspectInterface(selected.front());current.access=AccessMode::Shared;
        if(current!=b.binding)throw ProtocolError("approved pulse binding changed");
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(current.container);lockStatus="acquired";
        writeNew(executable().parent_path()/("grip-pulse-used-"+approvalHash+".asb"),approvalHash+"\n");
        auth.check(unixSeconds());io=openGripPulseTransport(auth,unixSeconds,[wait]{return wait->cancelled();});session=std::make_unique<Session>(*io,evidence);
        result=runAuthorizedGripPulse(*session,auth,unixSeconds,{[wait](Time due){wait->waitUntil(due);},[wait]{return wait->cancelled();}});
        if(result.complete())writeNew(out/"grip-baseline.asb",encodeGripBaseline(b));
    }catch(const std::exception& e){result.failure=e.what();}
    if(WaitForSingleObject(cancellation,0)==WAIT_OBJECT_0)result.cancelled=true;
    if(io&&!finishNative(*io,out))result.failure="native I/O finalization incomplete: "+result.failure;
    if(!evidence.finish())result.failure="trace incomplete: "+result.failure;
    result.deviceStateUncertain=session&&session->uncertain();
    writeNew(out/"grip-pulse.json",pulseResultJson(result));
    finalize(out,result.complete(),result.failure,true,result.deviceStateUncertain,AccessMode::Shared,lockStatus,session?session->queryAttempts():0,session?session->actuatorAttempts():0,false,gripPulseScope);
    return result.complete()?2:1;
}
int gripLifecycleWorker(const std::filesystem::path& reviewPath,const std::filesystem::path& approvalPath,const std::filesystem::path& out,const std::string& reviewHash,const std::string& approvalHash) {
    Evidence evidence(out);GripLifecycleResult result;std::string lockStatus="not_attempted";
    std::unique_ptr<ExperimentLock> lock;std::unique_ptr<WindowsTransport> io;std::unique_ptr<Session> session;
    try {
        const auto review=readText(reviewPath),approvalText=readText(approvalPath);
        if(hashText(review)!=reviewHash||hashText(approvalText)!=approvalHash)throw ProtocolError("lifecycle worker inputs changed");
        const auto baseline=checkedLifecycleReview(review);
        const auto auth=GripLifecycleAuthorization::approve(baseline,decodeGripLifecycleApproval(approvalText),hashText(review),unixSeconds());
        writeNew(out/"review.json",review);writeNew(out/"approval.asb",approvalText);
        std::vector<asb::HidDeviceInfo> selected;for(const auto& d:vendorInterfaces())if(utf8(d.instanceId)==baseline.binding.instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("approved exact vendor instance absent or ambiguous");
        auto current=inspectInterface(selected.front());current.access=AccessMode::Shared;
        if(current!=baseline.binding)throw ProtocolError("approved lifecycle interface/container/layout changed");
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(current.container);lockStatus="acquired";
        // Container-locked CREATE_NEW claim binds replay prevention to token bytes,
        // independent of the approval filename. Failure consumes the approval too.
        writeNew(executable().parent_path()/("grip-lifecycle-used-"+approvalHash+".asb"),approvalHash+"\n");
        auth.check(unixSeconds());io=openGripLifecycleTransport(auth,unixSeconds);session=std::make_unique<Session>(*io,evidence);
        result=runAuthorizedGripLifecycle(*session,auth,unixSeconds);
        if(lifecycleComplete(result))writeNew(out/"grip-baseline.asb",encodeGripBaseline(baseline));
    }catch(const std::exception& e){result.failure=e.what();}
    if(io&&!finishNative(*io,out))result.failure="native I/O finalization incomplete: "+result.failure;
    if(!evidence.finish())result.failure="trace incomplete: "+result.failure;
    result.deviceStateUncertain=session&&session->uncertain();writeLifecycle(out,result);
    finalize(out,lifecycleComplete(result),result.failure,true,result.deviceStateUncertain,AccessMode::Shared,lockStatus,session?session->queryAttempts():0,session?session->actuatorAttempts():0,false,gripLifecycleScope);
    return lifecycleComplete(result)?2:1;
}
Approval interactiveCheckpoints(const std::string& review,bool grip=false) {
    DWORD mode=0;if(!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode))throw ProtocolError("approval requires an interactive console; redirected consent refused");
    std::cout<<review<<"\nNEUTRAL LIFECYCLE ONLY. Mode entry/restoration may affect motors even with neutral samples.\n";
    auto confirm=[](const char* question){std::cout<<question<<" Type YES: "<<std::flush;std::string answer;if(!std::getline(std::cin,answer)||answer!="YES")throw ProtocolError("operator did not confirm");return true;};
    Approval a;a.manifestHash=hashText(review);
    a.directUsb=confirm("Direct USB cable connected and receiver unplugged?");
    a.observer=confirm("You are observing this exact controller throughout the test?");
    a.powerOffReady=confirm("You can promptly power off/disconnect the controller if state becomes uncertain?");
    a.normalVibration=confirm("Ordinary input and normal vibration have been independently checked before this experiment?");
    a.knownWritersQuiesced=confirm("Known competing controller writers are not intentionally active?");
    a.backgroundRiskAccepted=confirm("Accept unresolved background handle ownership and same-opcode reply attribution risk?");
    a.allDisabledAccepted=confirm("Accept that the all-disabled frame is not vendor-validated?");
    a.restorationRiskAccepted=confirm("Reviewed and accept the unresolved historical restoration errors for this exact manifest?");
    a.failStopAccepted=confirm("Accept fail-stop: any unexpected failure stops all traffic, including cleanup; operator power-off may be required?");
    if(grip)confirm("Accept grip-only audit: RAM 1/4/5 are NOT acquired; reported CRC16s do not prove full configuration preservation?");
    confirm("Reviewed two consistent physical readbacks and a successful offline rehearsal of this exact manifest?");
    std::cout<<"Approve exactly this manifest by typing its full SHA256: "<<a.manifestHash<<"\n> "<<std::flush;
    std::string typed;if(!std::getline(std::cin,typed)||typed!=a.manifestHash)throw ProtocolError("manifest hash not confirmed");
    a.approved=true;a.confirmedUnixSeconds=unixSeconds();return a;
}
Approval interactiveApproval(const Snapshot& baseline,const std::string& review) {
    if(baseline.schema!=2||(baseline.binding.access!=AccessMode::Exclusive&&baseline.binding.access!=AccessMode::Shared))throw ProtocolError("approval requires physical v2 baseline");
    return interactiveCheckpoints(review);
}
GripApproval interactiveGripApproval(const GripBaseline& baseline,const std::string& review) {
    validateGripBaseline(baseline);if(!baseline.physicalOrigin)throw ProtocolError("grip approval requires a physical baseline");
    return {interactiveCheckpoints(review,true),true};
}
GripRestoreApproval interactiveRestoreApproval(const GripBaseline& baseline,const std::string& review,bool observe=false) {
    validateGripBaseline(baseline);if(!baseline.physicalOrigin)throw ProtocolError("restore-only approval requires a physical baseline");
    DWORD mode=0;if(!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode))throw ProtocolError("approval requires an interactive console; redirected consent refused");
    std::cout<<review<<"\nONE LEFT-GRIP RESTORE COMMAND. No streaming entry, waveform, combined exit or right restore.\n";
    auto a=collectRestoreConfirmations(std::cin,std::cout,hashText(review),observe);
    a.confirmedUnixSeconds=unixSeconds();return a;
}
int neutralWorker(const std::filesystem::path& reviewPath,const std::filesystem::path& approvalPath,const std::filesystem::path& out,const std::string& reviewHash,const std::string& approvalHash) {
    Evidence evidence(out);bool complete=false;std::string error,lockStatus="not_attempted";AccessMode access=AccessMode::Unknown;
    std::unique_ptr<ExperimentLock> lock;std::unique_ptr<WindowsTransport> io;std::unique_ptr<Session> session;
    try {
        const auto review=readText(reviewPath),approvalText=readText(approvalPath);
        if(hashText(review)!=reviewHash||hashText(approvalText)!=approvalHash)throw ProtocolError("worker inputs changed after parent validation");
        const auto baseline=checkedReview(review);access=baseline.binding.access;
        const auto auth=NeutralAuthorization::approve(baseline,decodeApproval(approvalText),hashText(review),unixSeconds());
        writeNew(out/"review.json",review);writeNew(out/"approval.asb",approvalText);
        std::vector<asb::HidDeviceInfo> selected;for(const auto& d:vendorInterfaces())if(utf8(d.instanceId)==baseline.binding.instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("approved exact vendor instance absent or ambiguous");
        auto current=inspectInterface(selected.front());current.access=access;
        if(current!=baseline.binding)throw ProtocolError("approved interface/container/layout changed");
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(current.container);lockStatus="acquired";
        auth.check(unixSeconds());io=openNeutralTransport(auth,unixSeconds);session=std::make_unique<Session>(*io,evidence);
        const auto result=runAuthorizedNeutral(*session,auth,unixSeconds);complete=result.complete;error=result.failure;
        if(complete)writeNew(out/"snapshot.asb",encodeSnapshot(baseline));
    }catch(const std::exception& e){error=e.what();complete=false;}
    if(io&&!finishNative(*io,out)){complete=false;error="native I/O finalization incomplete: "+error;}
    if(!evidence.finish()){complete=false;error="trace incomplete: "+error;}
    finalize(out,complete,error,true,session&&session->uncertain(),access,lockStatus,session?session->queryAttempts():0,session?session->actuatorAttempts():0);
    return complete?0:1;
}
int gripNeutralWorker(const std::filesystem::path& reviewPath,const std::filesystem::path& approvalPath,const std::filesystem::path& out,const std::string& reviewHash,const std::string& approvalHash) {
    Evidence evidence(out);bool complete=false;std::string error,lockStatus="not_attempted";
    std::unique_ptr<ExperimentLock> lock;std::unique_ptr<WindowsTransport> io;std::unique_ptr<Session> session;
    try {
        const auto review=readText(reviewPath),approvalText=readText(approvalPath);
        if(hashText(review)!=reviewHash||hashText(approvalText)!=approvalHash)throw ProtocolError("worker inputs changed after validation");
        const auto baseline=checkedGripReview(review);
        const auto auth=GripNeutralAuthorization::approve(baseline,decodeGripApproval(approvalText),hashText(review),unixSeconds());
        writeNew(out/"review.json",review);writeNew(out/"approval.asb",approvalText);
        std::vector<asb::HidDeviceInfo> selected;for(const auto& d:vendorInterfaces())if(utf8(d.instanceId)==baseline.binding.instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("approved exact vendor instance absent or ambiguous");
        auto current=inspectInterface(selected.front());current.access=AccessMode::Shared;
        if(current!=baseline.binding)throw ProtocolError("approved grip interface/container/layout changed");
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(current.container);lockStatus="acquired";
        auth.check(unixSeconds());io=openGripNeutralTransport(auth,unixSeconds);session=std::make_unique<Session>(*io,evidence);
        const auto result=runAuthorizedGripNeutral(*session,auth,unixSeconds);complete=result.complete;error=result.failure;
        if(complete)writeNew(out/"grip-baseline.asb",encodeGripBaseline(baseline));
    }catch(const std::exception& e){error=e.what();complete=false;}
    if(io&&!finishNative(*io,out)){complete=false;error="native I/O finalization incomplete: "+error;}
    if(!evidence.finish()){complete=false;error="trace incomplete: "+error;}
    finalize(out,complete,error,true,session&&session->uncertain(),AccessMode::Shared,lockStatus,session?session->queryAttempts():0,session?session->actuatorAttempts():0,false,"grip-only");
    return complete?0:1;
}
int gripRestoreWorker(const std::filesystem::path& reviewPath,const std::filesystem::path& approvalPath,const std::filesystem::path& out,const std::string& reviewHash,const std::string& approvalHash,bool observe=false) {
    Evidence evidence(out);bool complete=false;std::string error,lockStatus="not_attempted";
    std::unique_ptr<ExperimentLock> lock;std::unique_ptr<WindowsTransport> io;std::unique_ptr<Session> session;
    try {
        const auto review=readText(reviewPath),approvalText=readText(approvalPath);
        if(hashText(review)!=reviewHash||hashText(approvalText)!=approvalHash)throw ProtocolError("restore worker inputs changed after validation");
        const auto baseline=checkedGripReview(review,true,observe);
        const auto auth=GripRestoreAuthorization::approve(baseline,decodeGripRestoreApproval(approvalText),hashText(review),unixSeconds(),observe);
        writeNew(out/"review.json",review);writeNew(out/"approval.asb",approvalText);
        std::vector<asb::HidDeviceInfo> selected;for(const auto& d:vendorInterfaces())if(utf8(d.instanceId)==baseline.binding.instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("approved exact vendor instance absent or ambiguous");
        auto current=inspectInterface(selected.front());current.access=AccessMode::Shared;
        if(current!=baseline.binding)throw ProtocolError("approved restore-only interface/container/layout changed");
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(current.container);lockStatus="acquired";
        auth.check(unixSeconds());io=openGripRestoreTransport(auth,unixSeconds);session=std::make_unique<Session>(*io,evidence);
        const auto result=runAuthorizedGripRestore(*session,auth,unixSeconds);complete=result.complete;error=result.failure;
        if(complete)writeNew(out/"grip-baseline.asb",encodeGripBaseline(baseline));
    }catch(const std::exception& e){error=e.what();complete=false;}
    if(io&&!finishNative(*io,out)){complete=false;error="native I/O finalization incomplete: "+error;}
    if(!evidence.finish()){complete=false;error="trace incomplete: "+error;}
    if(observe&&session) {
        auto capture=session->observation();
        if((io&&(!io->timingComplete()))||!evidence.healthy()||error.rfind("native I/O finalization incomplete:",0)==0||error.rfind("trace incomplete:",0)==0) {
            capture.complete=false;capture.error="capture finalization incomplete: "+error;
        }
        writeObservation(out,capture);
    }
    finalize(out,complete,error,true,session&&session->uncertain(),AccessMode::Shared,lockStatus,session?session->queryAttempts():0,session?session->actuatorAttempts():0,false,observe?"grip-left-restore-observe":"grip-left-restore-only");
    return complete?0:1;
}
#endif
std::wstring quote(const std::wstring& argument) {
    std::wstring result=L"\"";unsigned slashes=0;
    for(auto c:argument) {if(c==L'\\'){++slashes;continue;}if(c==L'"'){result.append(slashes*2+1,L'\\');result+=c;}else {result.append(slashes,L'\\');result+=c;}slashes=0;}
    result.append(slashes*2,L'\\');result+=L'"';return result;
}
int accessProbeWorker(const std::wstring& instance,const std::filesystem::path& out) {
    if(!std::filesystem::create_directory(out))throw ProtocolError("worker output directory must be new");
    bool complete=false;std::string error,lockStatus="not_attempted";std::unique_ptr<ExperimentLock> lock;
    try {
        const auto devices=vendorInterfaces();std::vector<asb::HidDeviceInfo> selected;
        for(const auto& d:devices)if(d.instanceId==instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("exact vendor instance is absent or ambiguous");
        const auto binding=inspectInterface(selected.front()); // metadata handle closes before trials
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(binding.container);lockStatus="acquired";
        const auto results=probeInterfaceAccess(binding);
        complete=results.size()==7;
        std::ostringstream report;report<<"{\"schema\":\"asb.apex6.access-probe.v1\",\"instance\":"<<json(binding.instance)
            <<",\"container\":"<<json(binding.container)<<",\"path\":"<<json(binding.path)
            <<",\"descriptor_signature\":"<<json(binding.descriptorSignature)
            <<",\"device_report_reads\":0,\"device_report_writes\":0,\"physical_actuation_attempted\":false,\"cases\":[";
        bool first=true;for(const auto& r:results) {
            if(r.opened&&!r.closed)complete=false;
            if(!first)report<<',';first=false;
            report<<"{\"name\":"<<json(r.name)<<",\"desired_access\":"<<r.access<<",\"share_mode\":"<<r.share
                <<",\"opened\":"<<(r.opened?"true":"false")<<",\"open_error\":"<<r.openError
                <<",\"closed\":"<<(r.closed?"true":"false")<<",\"close_error\":"<<r.closeError<<'}';
        }
        // Complete means the matrix was observed, not that any access succeeded.
        if(!results.empty()&&!results.back().opened&&results.back().openError!=ERROR_SHARING_VIOLATION&&results.back().openError!=ERROR_ACCESS_DENIED)complete=false;
        report<<"],\"complete\":"<<(complete?"true":"false")<<",\"safe_concurrent_writes_inferred\":false}\n";
        writeNew(out/"access-probe.json",report.str());
        if(!complete)error="access probe stopped on unexpected open or close failure";
    }catch(const std::exception& e){error=e.what();complete=false;}
    finalize(out,complete,error,false,false,AccessMode::Unknown,lockStatus);return complete?0:1;
}
int snapshotWorker(const std::wstring& instance,const std::filesystem::path& out,AccessMode access,bool grip=false) {
    Evidence evidence(out);bool complete=false;std::string error,lockStatus="not_attempted";
    std::unique_ptr<ExperimentLock> lock;std::unique_ptr<WindowsTransport> io;std::unique_ptr<Session> session;
    try {
        const auto devices=vendorInterfaces();std::vector<asb::HidDeviceInfo> selected;
        for(const auto& d:devices)if(d.instanceId==instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("exact vendor instance is absent or ambiguous");
        auto binding=inspectInterface(selected.front());binding.access=access;
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(binding.container);lockStatus="acquired";
        io=grip?openGripBaselineTransport(binding):openQueryTransport(binding);session=std::make_unique<Session>(*io,evidence);
        if(grip) {
            auto baseline=acquireGripBaseline(*session);
            if(!evidence.finish())throw ProtocolError("trace incomplete");
            writeNew(out/"grip-baseline.asb",encodeGripBaseline(baseline));
        } else {
            session->begin("readback",Time(30000000),128);auto snapshot=acquireSnapshot(*session);session->end();
            if(!evidence.finish())throw ProtocolError("trace incomplete");
            writeNew(out/"snapshot.asb",encodeSnapshot(snapshot));
        }
        complete=true;
    }catch(const std::exception& e){error=e.what();}
    if(io&&!finishNative(*io,out)){complete=false;error="native I/O finalization incomplete: "+error;}
    if(!evidence.finish()){complete=false;error="trace incomplete: "+error;}
    finalize(out,complete,error,true,false,access,lockStatus,session?session->queryAttempts():0,0,false,grip?"grip-only":"full-snapshot");return complete?0:1;
}
int listenWorker(const std::wstring& instance,const std::filesystem::path& out) {
    Evidence evidence(out);bool complete=false;std::string error,lockStatus="not_attempted";
    std::unique_ptr<ExperimentLock> lock;std::unique_ptr<WindowsTransport> io;
    try {
        std::vector<asb::HidDeviceInfo> selected;for(const auto& d:vendorInterfaces())if(d.instanceId==instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("exact vendor instance is absent or ambiguous");
        auto binding=inspectInterface(selected.front());binding.access=AccessMode::Shared;
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(binding.container);lockStatus="acquired";
        io=openInputListener(binding);const auto result=listenInput(*io,evidence,[]{Sleep(1);});
        complete=result.complete;error=result.error;
        std::ostringstream report;report<<"{\"schema\":\"asb.apex6.listen.v1\",\"instance\":"<<json(binding.instance)<<",\"container\":"<<json(binding.container)<<",\"path\":"<<json(binding.path)
            <<",\"descriptor_signature\":"<<json(binding.descriptorSignature)<<",\"max_seconds\":3,\"max_reports\":64,\"elapsed_us\":"<<result.elapsed.count()<<",\"reports\":"<<result.reports<<",\"stop\":"<<json(result.stop)
            <<",\"device_report_write_attempts\":0,\"reports_are_unsolicited\":true,\"background_writer_identified\":false,\"absence_of_background_traffic_proven\":false}\n";
        writeNew(out/"listen.json",report.str());
    }catch(const std::exception& e){complete=false;error=e.what();}
    if(io&&!finishNative(*io,out)){complete=false;error="native I/O finalization incomplete: "+error;}
    if(!evidence.finish()){complete=false;error="trace incomplete: "+error;}
    finalize(out,complete,error,true,false,AccessMode::Shared,lockStatus,0,0,true);return complete?0:1;
}
int ram5Worker(const std::wstring& instance,const std::filesystem::path& out) {
    Evidence evidence(out);bool complete=false;unsigned writes=0;std::string error,lockStatus="not_attempted";
    std::unique_ptr<ExperimentLock> lock;std::unique_ptr<WindowsTransport> io;
    try {
        std::vector<asb::HidDeviceInfo> selected;for(const auto& d:vendorInterfaces())if(d.instanceId==instance)selected.push_back(d);
        if(selected.size()!=1)throw ProtocolError("exact vendor instance is absent or ambiguous");
        auto binding=inspectInterface(selected.front());binding.access=AccessMode::Shared;
        lockStatus="failed";lock=std::make_unique<ExperimentLock>(binding.container);lockStatus="acquired";
        io=openRam5Diagnostic(binding);const auto result=examineRam5(*io,evidence,[]{Sleep(1);});
        complete=result.complete;error=result.error;writes=result.writes;
        std::ostringstream report;report<<"{\"schema\":\"asb.apex6.ram5-diagnostic.v1\",\"instance\":"<<json(binding.instance)<<",\"container\":"<<json(binding.container)
            <<",\"descriptor_signature\":"<<json(binding.descriptorSignature)<<",\"request_hex\":"<<json(hex(binding.layout.wrap(frame(0xa3,Bytes{1,5}))))
            <<",\"pre_listen_ms\":250,\"query_window_ms\":600,\"max_reports\":64,\"write_attempts\":"<<writes<<",\"reports_before\":"<<result.before<<",\"reports_after\":"<<result.after
            <<",\"matching_candidates\":"<<result.candidates<<",\"unexpected_reports\":"<<result.unexpected<<",\"stop\":"<<json(result.stop)
            <<",\"complete_means_capture_only\":true,\"response_attribution_proven\":false,\"baseline_acquired\":false,\"qualifies_neutral\":false}\n";
        writeNew(out/"ram5-diagnostic.json",report.str());
    }catch(const std::exception& e){complete=false;error=e.what();}
    if(io&&!finishNative(*io,out)){complete=false;error="native I/O finalization incomplete: "+error;}
    if(!evidence.finish()){complete=false;error="trace incomplete: "+error;}
    finalize(out,complete,error,true,false,AccessMode::Shared,lockStatus,writes);return complete?0:1;
}
int superviseSnapshot(const std::wstring& instance,const std::filesystem::path& output,bool selftest=false,bool simulateTimeout=false,bool accessProbe=false,AccessMode access=AccessMode::Exclusive,const std::wstring& neutralArgs={},bool readbackSelftest=false,bool listen=false,bool ram5=false,bool grip=false,bool restoreOnly=false,bool observe=false,bool lifecycle=false,bool pulse=false) {
    auto out=std::filesystem::absolute(output);
    if(!std::filesystem::create_directory(out))throw ProtocolError("output directory must be new");
    Handle job;job.h=CreateJobObjectW(nullptr,nullptr);if(!job.h)throw ProtocolError("job creation failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))throw ProtocolError("job setup failed");
    Handle parent;
    if(!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&parent.h,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0))throw ProtocolError("supervisor handle creation failed");
    const bool neutral=!neutralArgs.empty();
    PulseCancelConsole pulseCancel(pulse);
    auto exe=executable();auto command=quote(exe.wstring())+(neutral?std::wstring(pulse?L" --grip-pulse-worker ":lifecycle?L" --grip-lifecycle-worker ":observe?L" --grip-observe-worker ":restoreOnly?L" --grip-restore-worker ":grip?L" --grip-neutral-worker ":L" --neutral-worker ")+neutralArgs:selftest?(pulse?L" --grip-pulse-test-worker":grip?L" --grip-test-worker":L" --test-worker"):std::wstring(grip?L" --grip-baseline-worker --device ":ram5?L" --ram5-worker --device ":listen?L" --listen-worker --device ":accessProbe?L" --access-probe-worker --device ":L" --snapshot-worker --device ")+quote(instance))+L" --output "+quote((out/L"worker").wstring())+L" --supervisor-handle "+std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.h));
    if(readbackSelftest||(!neutral&&!selftest&&!accessProbe&&!listen&&!ram5))command+=L" --access "+wide(accessName(access));
    if(simulateTimeout&&!pulse)command+=L" --simulate-timeout";
    if(pulse) {
        command+=L" --cancel-handle "+std::to_wstring(reinterpret_cast<std::uintptr_t>(pulseCancel.event.h));
        if(selftest&&simulateTimeout)SetEvent(pulseCancel.event.h); // explicit cancel self-test, never hardware
    }
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&process))throw ProtocolError("worker launch failed");
    Handle child,thread;child.h=process.hProcess;thread.h=process.hThread;
    if(!AssignProcessToJobObject(job.h,child.h)||ResumeThread(thread.h)==DWORD(-1)){TerminateProcess(child.h,4);throw ProtocolError("worker supervision setup failed");}
    // Includes discovery/open/finalization; kernel termination is not guaranteed.
    const auto wait=WaitForSingleObject(child.h,neutral?80000:(listen||ram5)?10000:selftest?(simulateTimeout&&!pulse?250:10000):35000);bool uncertain=wait!=WAIT_OBJECT_0;
    bool terminationRequested=false,workerExited=wait==WAIT_OBJECT_0;
    if(uncertain){terminationRequested=TerminateProcess(child.h,4)!=FALSE;workerExited=WaitForSingleObject(child.h,1000)==WAIT_OBJECT_0;}
    DWORD code=4;if(workerExited)GetExitCodeProcess(child.h,&code);
    std::ostringstream status;status<<"{\"schema\":\"asb.apex6.supervisor.v2\",\"worker_exited\":"<<(workerExited?"true":"false")<<",\"deadline_or_wait_failure\":"<<(uncertain?"true":"false")<<",\"termination_requested\":"<<(terminationRequested?"true":"false")<<",\"exit_code\":"<<code<<",\"access_mode\":"<<json(neutral?accessName(access):selftest?"synthetic":accessProbe?"matrix":accessName(access))<<",\"physical_actuation_attempted\":"<<(neutral?"null":"false")<<",\"actuation_possible\":"<<(neutral?"true":"false")<<",\"recovery_not_inferred\":true}\n";
    writeNew(out/"supervisor.json",status.str());std::cout<<status.str();
    if(pulse) {
        const bool completed=!uncertain&&code==2;
        std::cerr<<(completed?"GRIP PULSE DIAGNOSTIC COMPLETE (exit 2): operator qualification pending. Report pulse location, strength, stop, input/vibration and unexpected behavior.\n":"GRIP PULSE FAILED/CANCELLED: disconnect/power off if physical; no retry or cleanup.\n");return completed?2:1;
    }
    if(lifecycle) {
        const bool completed=!uncertain&&code==2;
        std::cerr<<(completed?"GRIP LIFECYCLE DIAGNOSTIC COMPLETE (exit 2): physical recovery remains UNVERIFIED. Report ordinary input/vibration and unexpected movement; matching RAM is insufficient.\n":"GRIP LIFECYCLE FAILED: disconnect/power off; no retry or speculative cleanup.\n");
        return completed?2:1;
    }
    if(observe)std::cerr<<"RESTORE-OBSERVATION: restoration remains unresolved. Inspect capture evidence; power off/disconnect now. No retry or cleanup.\n";
    else if(neutral)std::cerr<<(restoreOnly?"RESTORE-ONLY: ":"NEUTRAL: ")<<(uncertain||code!=0?"RUN INCOMPLETE: actuator state may be uncertain. Power off/disconnect the controller; do not retry or send speculative cleanup.\n":"Protocol completed; physical recovery is NOT verified. Independently check ordinary input and normal vibration before any further experiment.\n");
    return !uncertain&&code==0?0:1;
}
struct Options {
    std::map<std::wstring,std::wstring> values;bool synthetic=false,direct=false,simulateTimeout=false;
    std::wstring get(const wchar_t* key)const{auto i=values.find(key);if(i==values.end())throw ProtocolError("missing required option");return i->second;}
    AccessMode access()const {auto it=values.find(L"--access");auto mode=it==values.end()?AccessMode::Exclusive:parseAccess(utf8(it->second));queryShareFlags(mode);return mode;}
};
Options parse(int argc,wchar_t** argv) {
    Options o;
    for(int i=2;i<argc;++i) {std::wstring key=argv[i];
        if(key==L"--synthetic"){if(o.synthetic)throw ProtocolError("duplicate option");o.synthetic=true;continue;}
        if(key==L"--direct-usb-confirmed"){if(o.direct)throw ProtocolError("duplicate option");o.direct=true;continue;}
        if(key==L"--simulate-timeout"){if(o.simulateTimeout)throw ProtocolError("duplicate option");o.simulateTimeout=true;continue;}
        if(key!=L"--device"&&key!=L"--output"&&key!=L"--snapshot"&&key!=L"--baseline"&&key!=L"--confirmation"&&key!=L"--manifest"&&key!=L"--supervisor-handle"&&key!=L"--access"&&key!=L"--approval"&&key!=L"--review-hash"&&key!=L"--approval-hash"&&key!=L"--cancel-handle")throw ProtocolError("unknown option");
        if(++i==argc||!o.values.emplace(key,argv[i]).second)throw ProtocolError("missing/duplicate option value");
    }return o;
}
void optionsOnly(const Options& o,std::initializer_list<const wchar_t*> allowed,bool synthetic=false,bool direct=false,bool timeout=false) {
    if((o.synthetic&&!synthetic)||(o.direct&&!direct)||(o.simulateTimeout&&!timeout))throw ProtocolError("option invalid for this command");
    for(const auto& [key,value]:o.values) {bool found=false;for(auto a:allowed)if(key==a)found=true;if(!found)throw ProtocolError("option invalid for this command");}
}
Snapshot baseline(const Options& o) {
    if(o.synthetic){if(o.values.contains(L"--snapshot"))throw ProtocolError("choose synthetic OR snapshot");return rehearsalFixture();}
    return decodeSnapshot(readText(o.get(L"--snapshot")));
}
GripBaseline gripBaseline(const Options& o) {
    if(o.synthetic){if(o.values.contains(L"--baseline"))throw ProtocolError("choose synthetic OR baseline");return gripRehearsalFixture();}
    return decodeGripBaseline(readText(o.get(L"--baseline")));
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc<2)throw ProtocolError("expected command: list, listen, examine-ram5, access-probe, snapshot, prepare, rehearse; physical execute is locked");
        const std::wstring command=argv[1];
        // Refuse before argument parsing, discovery, files, approval or device opens.
        if(command==L"execute")throw ProtocolError("PHYSICAL EXECUTION LOCKED: fresh wired baseline and neutral lifecycle review are still required");
#ifndef ASB_APEX6_NEUTRAL_RUNNER
        if(command==L"approve-grip-pulse"||command==L"execute-grip-pulse"||command==L"--grip-pulse-worker")throw ProtocolError("PHYSICAL EXECUTION LOCKED: query executable has no pulse adapter");
        if(command==L"approve-grip-lifecycle"||command==L"execute-grip-lifecycle")throw ProtocolError("PHYSICAL EXECUTION LOCKED: query executable has no actuator adapter");
        if(command==L"approve-neutral"||command==L"execute-neutral"||command==L"--neutral-worker"||command==L"approve-grip"||command==L"execute-grip"||command==L"--grip-neutral-worker"||command==L"approve-grip-restore"||command==L"execute-grip-restore"||command==L"--grip-restore-worker"||command==L"approve-grip-observe"||command==L"execute-grip-observe"||command==L"--grip-lifecycle-worker"||command==L"--grip-observe-worker")throw ProtocolError("PHYSICAL EXECUTION LOCKED: query executable has no actuator adapter");
#endif
        auto o=parse(argc,argv);
        if(command==L"prepare-grip-pulse"||command==L"rehearse-grip-pulse") {
            const bool prepare=command==L"prepare-grip-pulse";
            optionsOnly(o,prepare?std::initializer_list<const wchar_t*>{L"--baseline",L"--output"}:std::initializer_list<const wchar_t*>{L"--baseline",L"--output",L"--manifest"},true);
            const auto b=gripBaseline(o),reviewBaseline=b;const auto review=pulseReview(b);
            if(prepare){writeNew(o.get(L"--output"),review);std::cout<<"Pulse review generated; no device access. SHA256: "<<hashText(review)<<'\n';return 0;}
            if(o.values.contains(L"--manifest")&&checkedPulseReview(readText(o.get(L"--manifest")))!=reviewBaseline)throw ProtocolError("pulse baseline/review mismatch");
            return pulseRehearsal(b,review,o.get(L"--output"));
        }
        if(command==L"selftest-grip-pulse"||command==L"selftest-grip-pulse-cancel") {
            optionsOnly(o,{L"--output"});return superviseSnapshot({},o.get(L"--output"),true,command==L"selftest-grip-pulse-cancel",false,AccessMode::Synthetic,{},false,false,false,false,false,false,false,true);
        }
        if(command==L"--grip-pulse-worker"||command==L"--grip-pulse-test-worker") {
            const bool test=command==L"--grip-pulse-test-worker";
            if(test)optionsOnly(o,{L"--output",L"--supervisor-handle",L"--cancel-handle"});
            else optionsOnly(o,{L"--output",L"--supervisor-handle",L"--cancel-handle",L"--manifest",L"--approval",L"--review-hash",L"--approval-hash"});
            auto handle=[&](const wchar_t* key){auto text=o.get(key);std::size_t used=0;auto value=std::stoull(text,&used);if(!value||used!=text.size())throw ProtocolError("invalid inherited handle");return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));};
            Handle parent,cancel;parent.h=handle(L"--supervisor-handle");cancel.h=handle(L"--cancel-handle");BOOL inJob=FALSE;DWORD flags=0;
            if(!GetProcessId(parent.h)||GetProcessId(parent.h)==GetCurrentProcessId()||WaitForSingleObject(parent.h,0)!=WAIT_TIMEOUT||!IsProcessInJob(GetCurrentProcess(),nullptr,&inJob)||!inJob||!GetHandleInformation(cancel.h,&flags)||WaitForSingleObject(cancel.h,0)==WAIT_FAILED)throw ProtocolError("pulse worker requires live supervisor, job and cancel event");
            if(test){const auto b=gripRehearsalFixture();return pulseRehearsal(b,pulseReview(b),o.get(L"--output"),cancel.h);}
#ifdef ASB_APEX6_NEUTRAL_RUNNER
            return gripPulseWorker(o.get(L"--manifest"),o.get(L"--approval"),o.get(L"--output"),utf8(o.get(L"--review-hash")),utf8(o.get(L"--approval-hash")),cancel.h);
#endif
        }
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        if(command==L"approve-grip-pulse") {
            optionsOnly(o,{L"--manifest",L"--output"});const auto output=o.get(L"--output");if(std::filesystem::exists(output))throw ProtocolError("approval output must be new");
            const auto review=readText(o.get(L"--manifest"));const auto b=checkedPulseReview(review);if(!b.physicalOrigin)throw ProtocolError("pulse approval requires fresh physical baseline");
            DWORD mode=0;if(!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode))throw ProtocolError("approval requires an interactive console; redirected consent refused");
            std::cout<<review;auto a=collectPulseConfirmations(std::cin,std::cout,hashText(review));a.checkpoints.confirmedUnixSeconds=unixSeconds();
            GripPulseAuthorization::approve(b,a,hashText(review),unixSeconds());writeNew(output,encodeGripPulseApproval(a));std::cout<<"Pulse approval recorded; five-minute expiry; single use. No hardware opened.\n";return 0;
        }
        if(command==L"execute-grip-pulse") {
            optionsOnly(o,{L"--manifest",L"--approval",L"--output"});
            const auto reviewPath=std::filesystem::absolute(o.get(L"--manifest")),approvalPath=std::filesystem::absolute(o.get(L"--approval"));
            const auto review=readText(reviewPath),approvalText=readText(approvalPath);const auto b=checkedPulseReview(review);
            GripPulseAuthorization::approve(b,decodeGripPulseApproval(approvalText),hashText(review),unixSeconds());
            if(std::filesystem::exists(executable().parent_path()/("grip-pulse-used-"+hashText(approvalText)+".asb")))throw ProtocolError("pulse approval already consumed");
            const auto args=L"--manifest "+quote(reviewPath.wstring())+L" --approval "+quote(approvalPath.wstring())+L" --review-hash "+wide(hashText(review))+L" --approval-hash "+wide(hashText(approvalText));
            return superviseSnapshot({},o.get(L"--output"),false,false,false,b.binding.access,args,false,false,false,false,false,false,false,true);
        }
#endif
        if(command==L"prepare-grip-lifecycle"||command==L"rehearse-grip-lifecycle") {
            const bool prepare=command==L"prepare-grip-lifecycle";
            optionsOnly(o,prepare?std::initializer_list<const wchar_t*>{L"--baseline",L"--output"}:std::initializer_list<const wchar_t*>{L"--baseline",L"--output",L"--manifest"},true);
            const auto s=gripBaseline(o);const auto review=lifecycleReviewManifest(s);
            if(prepare){writeNew(o.get(L"--output"),review);std::cout<<"Lifecycle review generated; no device access. SHA256: "<<hashText(review)<<'\n';return 0;}
            if(o.values.contains(L"--manifest")&&checkedLifecycleReview(readText(o.get(L"--manifest")))!=s)throw ProtocolError("lifecycle baseline/review mismatch");
            const std::filesystem::path out=o.get(L"--output");Evidence evidence(out);FakeIo io(s);Session session(io,evidence);
            // Rehearse the captured anomalous case explicitly; unit tests cover all ACK combinations.
            io.rawResponseHook=[](auto& fake,auto& wire){if(fake.writes==17||fake.writes==18)wire=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");};
            auto result=rehearseGripLifecycle(session,s);if(!evidence.finish())result.failure="trace incomplete";
            writeLifecycle(out,result);writeNew(out/"review.json",review);
            finalize(out,lifecycleComplete(result),result.failure,false,result.deviceStateUncertain,AccessMode::Synthetic,"not_required",session.queryAttempts(),session.actuatorAttempts(),false,gripLifecycleScope);
            std::cout<<(lifecycleComplete(result)?"GRIP LIFECYCLE DIAGNOSTIC COMPLETE (exit 2): physical recovery remains UNVERIFIED; 32 fake writes; zero device opens\n":"Grip lifecycle rehearsal FAILED\n");return lifecycleComplete(result)?2:1;
        }
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        if(command==L"approve-grip-lifecycle") {
            optionsOnly(o,{L"--manifest",L"--output"});const auto output=o.get(L"--output");
            if(std::filesystem::exists(output))throw ProtocolError("approval output must be new");
            const auto review=readText(o.get(L"--manifest"));const auto s=checkedLifecycleReview(review);
            if(!s.physicalOrigin)throw ProtocolError("lifecycle approval requires fresh physical baseline");
            DWORD mode=0;if(!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode))throw ProtocolError("approval requires an interactive console; redirected consent refused");
            std::cout<<review;auto a=collectLifecycleConfirmations(std::cin,std::cout,hashText(review));a.checkpoints.confirmedUnixSeconds=unixSeconds();
            GripLifecycleAuthorization::approve(s,a,hashText(review),unixSeconds());writeNew(output,encodeGripLifecycleApproval(a));
            std::cout<<"Lifecycle approval recorded; five-minute expiry; single use. No hardware opened.\n";return 0;
        }
        if(command==L"execute-grip-lifecycle") {
            optionsOnly(o,{L"--manifest",L"--approval",L"--output"});
            const auto reviewPath=std::filesystem::absolute(o.get(L"--manifest")),approvalPath=std::filesystem::absolute(o.get(L"--approval"));
            const auto review=readText(reviewPath),approvalText=readText(approvalPath);const auto s=checkedLifecycleReview(review);
            GripLifecycleAuthorization::approve(s,decodeGripLifecycleApproval(approvalText),hashText(review),unixSeconds());
            if(std::filesystem::exists(executable().parent_path()/("grip-lifecycle-used-"+hashText(approvalText)+".asb")))throw ProtocolError("lifecycle approval already consumed");
            const auto args=L"--manifest "+quote(reviewPath.wstring())+L" --approval "+quote(approvalPath.wstring())+L" --review-hash "+wide(hashText(review))+L" --approval-hash "+wide(hashText(approvalText));
            return superviseSnapshot({},o.get(L"--output"),false,false,false,s.binding.access,args,false,false,false,true,false,false,true);
        }
#endif
        if(command==L"grip-baseline") {
            optionsOnly(o,{L"--device",L"--output",L"--access"},false,true);
            if(!o.direct||!o.values.contains(L"--access")||o.access()!=AccessMode::Shared)throw ProtocolError("grip readback requires --direct-usb-confirmed and explicit --access shared");
            return superviseSnapshot(o.get(L"--device"),o.get(L"--output"),false,false,false,AccessMode::Shared,{},false,false,false,true);
        }
        if(command==L"selftest-grip-baseline") {
            optionsOnly(o,{L"--output"});return superviseSnapshot({},o.get(L"--output"),true,false,false,AccessMode::Synthetic,{},false,false,false,true);
        }
        if(command==L"compare-grip") {
            optionsOnly(o,{L"--baseline",L"--confirmation"});const auto a=gripBaseline(o),b=decodeGripBaseline(readText(o.get(L"--confirmation")));
            if(!a.physicalOrigin||!b.physicalOrigin||a!=b)throw ProtocolError("two matching physical grip baselines required");
            std::cout<<"Grip-scoped baselines match; RAM 1/4/5 unacquired. Separate-run evidence must still be reviewed. No actuation authorized.\n";return 0;
        }
        if(command==L"prepare-grip"||command==L"rehearse-grip"||command==L"prepare-grip-restore"||command==L"rehearse-grip-restore"||command==L"prepare-grip-observe"||command==L"rehearse-grip-observe") {
            const bool observe=command==L"prepare-grip-observe"||command==L"rehearse-grip-observe";
            const bool restoreOnly=observe||command==L"prepare-grip-restore"||command==L"rehearse-grip-restore";
            const bool prepare=command==L"prepare-grip"||command==L"prepare-grip-restore"||command==L"prepare-grip-observe";
            optionsOnly(o,prepare?std::initializer_list<const wchar_t*>{L"--baseline",L"--output"}:std::initializer_list<const wchar_t*>{L"--baseline",L"--output",L"--manifest"},true);
            const auto s=gripBaseline(o);const auto review=gripReviewManifest(s,restoreOnly,observe);
            if(prepare){writeNew(o.get(L"--output"),review);std::cout<<"Grip review generated; no approval or device execution. SHA256: "<<hashText(review)<<'\n';return 0;}
            if(o.values.contains(L"--manifest")&&checkedGripReview(readText(o.get(L"--manifest")),restoreOnly,observe)!=s)throw ProtocolError("grip review/baseline/source/executable mismatch");
            const std::filesystem::path out=o.get(L"--output");Evidence evidence(out);FakeIo io(s);Session session(io,evidence);
            unsigned modeReads=0;
            if(observe) {
                // Explicit synthetic scenario, not a prediction of device behavior:
                // recorded zero-count envelope, then one normal ACK-shaped frame.
                io.rawResponseHook=[](auto&,auto& wire){if(wire[3]==0x53)wire=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");};
                io.readHook=[&](auto& fake,Time,bool poll)->std::optional<IoResult> {
                    if(fake.writes==15&&!poll&&++modeReads==2)fake.incoming.push_back(unhex("005aa5530100000000000000000000000000000000000000000000000000000054"));
                    return {};
                };
            }
            auto result=restoreOnly?rehearseGripRestore(session,s,observe):rehearseNeutral(session,s);
            const auto traceComplete=evidence.finish();if(!traceComplete){result.complete=false;result.failure="trace incomplete";}
            if(observe)writeObservation(out,session.observation());
            auto saved=s;saved.physicalOrigin=false;saved.binding.access=AccessMode::Synthetic;
            writeNew(out/"grip-baseline.asb",encodeGripBaseline(saved));writeNew(out/"review.json",review);
            finalize(out,result.complete,result.failure,false,false,AccessMode::Synthetic,"not_required",session.queryAttempts(),session.actuatorAttempts(),false,observe?"grip-left-restore-observe":restoreOnly?"grip-left-restore-only":"grip-only");
            const bool rehearsed=traceComplete&&(observe?session.observation().complete:result.complete);
            std::cout<<"Grip-only offline rehearsal: "<<(rehearsed?"complete":"FAILED")<<"; "<<io.writes<<" fake requests; zero device opens\n";return rehearsed?0:1;
        }
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        if(command==L"approve-grip-restore"||command==L"approve-grip-observe") {
            const bool observe=command==L"approve-grip-observe";
            optionsOnly(o,{L"--manifest",L"--output"});const auto output=o.get(L"--output");
            if(std::filesystem::exists(output))throw ProtocolError("approval output must be new");
            const auto review=readText(o.get(L"--manifest"));const auto s=checkedGripReview(review,true,observe);
            writeNew(output,encodeGripRestoreApproval(interactiveRestoreApproval(s,review,observe)));
            std::cout<<"Restore-only approval recorded; expires in five minutes. No hardware opened.\n";return 0;
        }
        if(command==L"execute-grip-restore"||command==L"execute-grip-observe") {
            const bool observe=command==L"execute-grip-observe";
            optionsOnly(o,{L"--manifest",L"--approval",L"--output"});
            const auto reviewPath=std::filesystem::absolute(o.get(L"--manifest")),approvalPath=std::filesystem::absolute(o.get(L"--approval"));
            const auto review=readText(reviewPath),approvalText=readText(approvalPath);const auto s=checkedGripReview(review,true,observe);
            GripRestoreAuthorization::approve(s,decodeGripRestoreApproval(approvalText),hashText(review),unixSeconds(),observe);
            const auto args=L"--manifest "+quote(reviewPath.wstring())+L" --approval "+quote(approvalPath.wstring())+L" --review-hash "+wide(hashText(review))+L" --approval-hash "+wide(hashText(approvalText));
            return superviseSnapshot({},o.get(L"--output"),false,false,false,s.binding.access,args,false,false,false,true,true,observe);
        }
        if(command==L"approve-grip") {
            optionsOnly(o,{L"--manifest",L"--output"});const auto output=o.get(L"--output");
            if(std::filesystem::exists(output))throw ProtocolError("approval output must be new");
            const auto review=readText(o.get(L"--manifest"));const auto s=checkedGripReview(review);
            writeNew(output,encodeGripApproval(interactiveGripApproval(s,review)));
            std::cout<<"Grip-neutral approval recorded; expires in five minutes. No hardware opened.\n";return 0;
        }
        if(command==L"execute-grip") {
            optionsOnly(o,{L"--manifest",L"--approval",L"--output"});
            const auto reviewPath=std::filesystem::absolute(o.get(L"--manifest")),approvalPath=std::filesystem::absolute(o.get(L"--approval"));
            const auto review=readText(reviewPath),approvalText=readText(approvalPath);const auto s=checkedGripReview(review);
            GripNeutralAuthorization::approve(s,decodeGripApproval(approvalText),hashText(review),unixSeconds());
            const auto args=L"--manifest "+quote(reviewPath.wstring())+L" --approval "+quote(approvalPath.wstring())+L" --review-hash "+wide(hashText(review))+L" --approval-hash "+wide(hashText(approvalText));
            return superviseSnapshot({},o.get(L"--output"),false,false,false,s.binding.access,args,false,false,false,true);
        }
        if(command==L"approve-neutral") {
            optionsOnly(o,{L"--manifest",L"--output"});const auto output=o.get(L"--output");
            if(std::filesystem::exists(output))throw ProtocolError("approval output must be new");
            const auto review=readText(o.get(L"--manifest"));const auto s=checkedReview(review);
            auto approval=interactiveApproval(s,review);writeNew(output,encodeApproval(approval));
            std::cout<<"Approval recorded for this exact neutral manifest; expires in five minutes. No hardware opened.\n";return 0;
        }
        if(command==L"execute-neutral") {
            optionsOnly(o,{L"--manifest",L"--approval",L"--output"});
            const auto reviewPath=std::filesystem::absolute(o.get(L"--manifest")),approvalPath=std::filesystem::absolute(o.get(L"--approval"));
            const auto review=readText(reviewPath),approvalText=readText(approvalPath);const auto s=checkedReview(review);
            NeutralAuthorization::approve(s,decodeApproval(approvalText),hashText(review),unixSeconds());
            auto args=L"--manifest "+quote(reviewPath.wstring())+L" --approval "+quote(approvalPath.wstring())+L" --review-hash "+wide(hashText(review))+L" --approval-hash "+wide(hashText(approvalText));
            return superviseSnapshot({},o.get(L"--output"),false,false,false,s.binding.access,args);
        }
#endif
        if(command==L"list") {
            optionsOnly(o,{});
            for(const auto& d:vendorInterfaces())std::cout<<"{\"instance\":"<<json(utf8(d.instanceId))<<",\"container\":"<<json(utf8(d.containerId))<<",\"product_id\":"<<d.productId<<",\"input_length\":"<<d.inputReportLength<<",\"output_length\":"<<d.outputReportLength<<"}\n";
            return 0;
        }
        if(command==L"snapshot"||command==L"access-probe"||command==L"listen"||command==L"examine-ram5") {
            if(command==L"snapshot")optionsOnly(o,{L"--device",L"--output",L"--access"},false,true);
            else optionsOnly(o,{L"--device",L"--output"},false,true);
            if(!o.direct)throw ProtocolError("confirm direct USB cable with receiver disconnected using --direct-usb-confirmed");
            return superviseSnapshot(o.get(L"--device"),o.get(L"--output"),false,false,command==L"access-probe",(command==L"listen"||command==L"examine-ram5")?AccessMode::Shared:o.access(),{},false,command==L"listen",command==L"examine-ram5");
        }
        if(command==L"selftest-supervisor") {
            optionsOnly(o,{L"--output"},false,false,true);return superviseSnapshot({},o.get(L"--output"),true,o.simulateTimeout);
        }
        if(command==L"selftest-readback") {
            optionsOnly(o,{L"--output",L"--access"});return superviseSnapshot({},o.get(L"--output"),true,false,false,o.access(),{},true);
        }
        if(command==L"--snapshot-worker"||command==L"--test-worker"||command==L"--grip-baseline-worker"||command==L"--grip-test-worker"||command==L"--access-probe-worker"||command==L"--neutral-worker"||command==L"--grip-neutral-worker"||command==L"--grip-restore-worker"||command==L"--grip-lifecycle-worker"||command==L"--grip-observe-worker"||command==L"--listen-worker"||command==L"--ram5-worker") {
            const bool test=command==L"--test-worker"||command==L"--grip-test-worker";
            if(command==L"--neutral-worker"||command==L"--grip-neutral-worker"||command==L"--grip-restore-worker"||command==L"--grip-lifecycle-worker"||command==L"--grip-observe-worker")optionsOnly(o,{L"--output",L"--supervisor-handle",L"--manifest",L"--approval",L"--review-hash",L"--approval-hash"});
            else if(command==L"--snapshot-worker"||command==L"--grip-baseline-worker")optionsOnly(o,{L"--device",L"--output",L"--supervisor-handle",L"--access"});
            else if(test)optionsOnly(o,{L"--output",L"--supervisor-handle",L"--access"},false,false,true);
            else optionsOnly(o,{L"--device",L"--output",L"--supervisor-handle"},false,false,test);
            const auto value=o.get(L"--supervisor-handle");std::size_t used=0;auto raw=std::stoull(value,&used);
            Handle parent;parent.h=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw));BOOL inJob=FALSE;
            if(used!=value.size()||!raw||!GetProcessId(parent.h)||GetProcessId(parent.h)==GetCurrentProcessId()||WaitForSingleObject(parent.h,0)!=WAIT_TIMEOUT||!IsProcessInJob(GetCurrentProcess(),nullptr,&inJob)||!inJob)
                throw ProtocolError("snapshot worker requires a live supervisor and job");
#ifdef ASB_APEX6_NEUTRAL_RUNNER
            if(command==L"--neutral-worker")return neutralWorker(o.get(L"--manifest"),o.get(L"--approval"),o.get(L"--output"),utf8(o.get(L"--review-hash")),utf8(o.get(L"--approval-hash")));
            if(command==L"--grip-neutral-worker")return gripNeutralWorker(o.get(L"--manifest"),o.get(L"--approval"),o.get(L"--output"),utf8(o.get(L"--review-hash")),utf8(o.get(L"--approval-hash")));
            if(command==L"--grip-restore-worker")return gripRestoreWorker(o.get(L"--manifest"),o.get(L"--approval"),o.get(L"--output"),utf8(o.get(L"--review-hash")),utf8(o.get(L"--approval-hash")));
            if(command==L"--grip-lifecycle-worker")return gripLifecycleWorker(o.get(L"--manifest"),o.get(L"--approval"),o.get(L"--output"),utf8(o.get(L"--review-hash")),utf8(o.get(L"--approval-hash")));
            if(command==L"--grip-observe-worker")return gripRestoreWorker(o.get(L"--manifest"),o.get(L"--approval"),o.get(L"--output"),utf8(o.get(L"--review-hash")),utf8(o.get(L"--approval-hash")),true);
#endif
            if(test) {
                if(command==L"--grip-test-worker") {
                    optionsOnly(o,{L"--output",L"--supervisor-handle"});const std::filesystem::path out=o.get(L"--output");
                    Evidence evidence(out);FakeIo io(gripRehearsalFixture());Session session(io,evidence);const auto saved=acquireGripBaseline(session);
                    writeNew(out/"grip-baseline.asb",encodeGripBaseline(saved));const bool complete=evidence.finish();
                    finalize(out,complete,complete?"":"trace incomplete",false,false,AccessMode::Synthetic,"synthetic_worker_no_lock",session.queryAttempts(),0,false,"grip-only");return complete?0:1;
                }
                if(o.simulateTimeout){Sleep(5000);return 3;}
                if(o.values.contains(L"--access")) {
                    const auto mode=o.access();auto fixture=rehearsalFixture();fixture.binding.access=mode;
                    const std::filesystem::path out=o.get(L"--output");Evidence evidence(out);FakeIo io(fixture);io.pretendPhysical=true;
                    Session session(io,evidence);session.begin("readback_selftest",Time(30000000),128);const auto observed=acquireSnapshot(session);session.end();
                    // Do not export a fake fixture as a physical acquisition.
                    auto saved=observed;saved.binding.access=AccessMode::Synthetic;writeNew(out/"snapshot.asb",encodeSnapshot(saved));
                    const bool complete=observed==fixture&&evidence.finish();
                    finalize(out,complete,complete?"":"selftest mismatch",false,false,mode,"synthetic_worker_no_lock",session.queryAttempts());return complete?0:1;
                }
                const std::filesystem::path out=o.get(L"--output");Evidence evidence(out);auto s=rehearsalFixture();FakeIo io(s);Session session(io,evidence);
                const auto result=rehearseNeutral(session,s);const bool complete=evidence.finish()&&result.complete;
                writeNew(out/"snapshot.asb",encodeSnapshot(s));finalize(out,complete,result.failure,false,false,s.binding.access,"not_required",session.queryAttempts(),session.actuatorAttempts());return complete?0:1;
            }
            if(command==L"--access-probe-worker")return accessProbeWorker(o.get(L"--device"),o.get(L"--output"));
            if(command==L"--listen-worker")return listenWorker(o.get(L"--device"),o.get(L"--output"));
            if(command==L"--ram5-worker")return ram5Worker(o.get(L"--device"),o.get(L"--output"));
            return snapshotWorker(o.get(L"--device"),o.get(L"--output"),o.access(),command==L"--grip-baseline-worker");
        }
        if(command==L"prepare") {
            optionsOnly(o,{L"--snapshot",L"--output"},true);auto s=baseline(o);const auto text=reviewManifest(s);
            writeNew(o.get(L"--output"),text);std::cout<<"Review generated; no approval or hardware execution. Manifest SHA256: "<<hashText(text)<<'\n';return 0;
        }
        if(command==L"rehearse") {
            optionsOnly(o,{L"--snapshot",L"--output",L"--manifest"},true);auto s=baseline(o);
            if(o.values.contains(L"--manifest")&&checkedReview(readText(o.get(L"--manifest")))!=s)throw ProtocolError("manifest/baseline/executable/source mismatch");
            const std::filesystem::path out=o.get(L"--output");Evidence evidence(out);FakeIo io(s);Session session(io,evidence);
            auto result=rehearseNeutral(session,s);if(!evidence.finish()){result.complete=false;result.failure="trace incomplete";}
            writeNew(out/"snapshot.asb",encodeSnapshot(s));finalize(out,result.complete,result.failure,false,false,s.binding.access,"not_required",session.queryAttempts(),session.actuatorAttempts());
            std::cout<<"Offline rehearsal: "<<(result.complete?"complete":"FAILED")<<"; "<<io.writes<<" fake requests; zero device opens\n";return result.complete?0:1;
        }
        throw ProtocolError("unknown command");
    }catch(const std::exception& e){std::cerr<<"Apex6 experiment refused: "<<e.what()<<'\n';return 1;}
}
