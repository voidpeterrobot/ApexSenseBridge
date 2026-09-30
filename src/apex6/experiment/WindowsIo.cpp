#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cfgmgr32.h>
#include <bcrypt.h>
#include "apex6/experiment/WindowsIo.h"
#include "apex6/experiment/WindowsIoTestHooks.h"
#ifdef ASB_APEX6_INTEGRATED
#include "apex6/dongle/Pulse.h"
#endif
#include "platform/HidDiscovery.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace asb::apex6::experiment {
namespace {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value!=nullptr)CloseHandle(value);}
};
void require(bool ok,const char* why){if(!ok)throw ProtocolError(std::string(why)+" (Win32 "+std::to_string(GetLastError())+")");}
Time monotonic(){return std::chrono::duration_cast<Time>(std::chrono::steady_clock::now().time_since_epoch());}
DWORD waitMs(Time deadline,Time now) {
    auto remaining=deadline-now;if(remaining.count()<=0)return 0;
    return static_cast<DWORD>(std::min<Time::rep>((remaining.count()+999)/1000,600));
}
WindowsIoHooks realCalls() {
    return {
        [](bool read,HANDLE h,void* p,DWORD n,OVERLAPPED* o,DWORD& error){auto ok=read?ReadFile(h,p,n,nullptr,o):WriteFile(h,p,n,nullptr,o);error=ok?ERROR_SUCCESS:GetLastError();return ok;},
        [](HANDLE h,DWORD ms){return WaitForSingleObject(h,ms);},
        [](HANDLE h,OVERLAPPED* o,DWORD& count,DWORD& error){auto ok=GetOverlappedResult(h,o,&count,FALSE);error=ok?ERROR_SUCCESS:GetLastError();return ok;},
        [](HANDLE h,OVERLAPPED* o){CancelIoEx(h,o);},
        []{return monotonic();}
    };
}
bool unresolvedCompletion(DWORD error) {
    return error==ERROR_IO_INCOMPLETE || error==ERROR_INVALID_HANDLE || error==ERROR_INVALID_PARAMETER;
}
Layout describe(HANDLE handle,std::string& signature) {
    struct Preparsed {PHIDP_PREPARSED_DATA value=nullptr;~Preparsed(){if(value)HidD_FreePreparsedData(value);}} data;
    require(HidD_GetPreparsedData(handle,&data.value)!=FALSE,"HidD_GetPreparsedData failed");
    HIDP_CAPS caps{};require(HidP_GetCaps(data.value,&caps)==HIDP_STATUS_SUCCESS,"HidP_GetCaps failed");
    require(caps.UsagePage==0xffa0,"not an FFA0 vendor collection");
    std::ostringstream header;header<<"windows-hid-caps-v1;"<<caps.UsagePage<<';'<<caps.Usage<<';'<<caps.InputReportByteLength<<';'<<caps.OutputReportByteLength<<';'<<caps.FeatureReportByteLength;
    std::vector<std::string> fields;
    std::set<unsigned> inputIds,outputIds;
    for(auto type:{HidP_Input,HidP_Output,HidP_Feature}) {
        const USHORT valueCount=type==HidP_Input?caps.NumberInputValueCaps:type==HidP_Output?caps.NumberOutputValueCaps:caps.NumberFeatureValueCaps;
        const USHORT buttonCount=type==HidP_Input?caps.NumberInputButtonCaps:type==HidP_Output?caps.NumberOutputButtonCaps:caps.NumberFeatureButtonCaps;
        require(valueCount<=1024&&buttonCount<=1024,"capability table too large");
        if(valueCount) {
            std::vector<HIDP_VALUE_CAPS> values(valueCount);USHORT count=valueCount;
            require(HidP_GetValueCaps(type,values.data(),&count,data.value)==HIDP_STATUS_SUCCESS&&count==valueCount,"value capabilities incomplete");
            for(const auto& v:values) {
                if(type==HidP_Input)inputIds.insert(v.ReportID);if(type==HidP_Output)outputIds.insert(v.ReportID);
                std::ostringstream f;f<<"v;"<<type<<';'<<unsigned(v.ReportID)<<';'<<v.UsagePage<<';'<<v.BitField<<';'<<v.BitSize<<';'<<v.ReportCount<<';'<<v.LinkCollection<<';'<<v.LinkUsage<<';'<<v.LinkUsagePage<<';'<<unsigned(v.IsAlias)<<';'<<unsigned(v.IsRange)<<';'<<unsigned(v.IsAbsolute)<<';'<<unsigned(v.HasNull)<<';'<<v.Units<<';'<<v.UnitsExp<<';'<<v.LogicalMin<<';'<<v.LogicalMax<<';'<<v.PhysicalMin<<';'<<v.PhysicalMax;
                if(v.IsRange)f<<';'<<v.Range.UsageMin<<';'<<v.Range.UsageMax<<';'<<v.Range.DataIndexMin<<';'<<v.Range.DataIndexMax;
                else f<<';'<<v.NotRange.Usage<<';'<<v.NotRange.DataIndex;
                fields.push_back(f.str());
            }
        }
        if(buttonCount) {
            std::vector<HIDP_BUTTON_CAPS> buttons(buttonCount);USHORT count=buttonCount;
            require(HidP_GetButtonCaps(type,buttons.data(),&count,data.value)==HIDP_STATUS_SUCCESS&&count==buttonCount,"button capabilities incomplete");
            for(const auto& b:buttons) {
                if(type==HidP_Input)inputIds.insert(b.ReportID);if(type==HidP_Output)outputIds.insert(b.ReportID);
                std::ostringstream f;f<<"b;"<<type<<';'<<unsigned(b.ReportID)<<';'<<b.UsagePage<<';'<<b.BitField<<';'<<b.LinkCollection<<';'<<b.LinkUsage<<';'<<b.LinkUsagePage<<';'<<unsigned(b.IsAlias)<<';'<<unsigned(b.IsRange)<<';'<<unsigned(b.IsAbsolute);
                if(b.IsRange)f<<';'<<b.Range.UsageMin<<';'<<b.Range.UsageMax<<';'<<b.Range.DataIndexMin<<';'<<b.Range.DataIndexMax;
                else f<<';'<<b.NotRange.Usage<<';'<<b.NotRange.DataIndex;
                fields.push_back(f.str());
            }
        }
    }
    require(inputIds.size()==1&&outputIds.size()==1,"ambiguous or missing vendor report IDs");
    std::sort(fields.begin(),fields.end());for(const auto& f:fields)header<<'|'<<f;
    signature=header.str();require(signature.size()<=16384,"descriptor signature too large");
    Layout layout{static_cast<std::uint8_t>(*inputIds.begin()),static_cast<std::uint8_t>(*outputIds.begin()),caps.InputReportByteLength,caps.OutputReportByteLength};layout.validate();return layout;
}
bool queryOnly(std::span<const std::uint8_t> wire,const Layout& layout) {
    if(wire.size()!=layout.outputLength||wire[0]!=layout.outputId)return false;
    if(wire[1]!=0x5a||wire[2]!=0xa5||wire[4]<2||wire[4]>29)return false;
    const auto cmd=wire[3];const auto n=wire[4]-2;
    bool allowed=((cmd==1||cmd==4||cmd==7||cmd==0xa1)&&n==0);
    if(cmd==0xa3&&(n==2||n==4)) {
        const auto id=wire[6];const bool known=id==1||id==4||id==5||id==6;
        allowed=known&&((n==2&&wire[5]==1)||(n==4&&wire[5]==0&&wire[8]==16));
    }
    if(!allowed)return false;
    return layout.wrap(frame(cmd,wire.subspan(5,n)))==Bytes(wire.begin(),wire.end());
}
// Every operation owns all memory the kernel can reference. If cancellation
// cannot be resolved, the worker intentionally retains it AND the handle until
// process teardown. No destructor may reclaim unresolved kernel-owned storage.
struct Operation {
    OVERLAPPED overlapped{};Bytes buffer;Handle event;std::uint64_t id=0;
    explicit Operation(std::size_t size):buffer(size){event.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(event.value!=nullptr,"event creation failed");overlapped.hEvent=event.value;}
};
class WindowsIo final:public WindowsTransport {
public:
    explicit WindowsIo(Binding binding,bool inputOnly=false,bool exclusiveLive=false):binding_(std::move(binding)),inputOnly_(inputOnly) {
        if(inputOnly_&&binding_.access!=AccessMode::Shared)throw ProtocolError("listener requires shared access");
        handle_=CreateFileW(wide(binding_.path).c_str(),inputOnly_?GENERIC_READ:GENERIC_READ|GENERIC_WRITE,exclusiveLive?0:queryShareFlags(binding_.access),nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        require(handle_!=INVALID_HANDLE_VALUE,"vendor open failed for requested access mode; no fallback");
        try {std::string signature;auto layout=describe(handle_,signature);require(layout==binding_.layout&&signature==binding_.descriptorSignature,"layout changed while opening");}
        catch(...){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;throw;}
    }
    WindowsIo(Binding binding,HANDLE handle,WindowsIoHooks hooks,bool inputOnly=false):binding_(std::move(binding)),handle_(handle),hooks_(std::move(hooks)),inputOnly_(inputOnly) {binding_.layout.validate();}
#ifdef ASB_APEX6_NEUTRAL_RUNNER
    void authorize(const GripPulseAuthorization& a,const std::function<std::int64_t()>& clock,const std::function<bool()>& cancelled) {
        a.check(clock());require(binding_==a.baseline().binding&&bool(cancelled),"native pulse binding/cancellation mismatch");
        a.consume();pulse_=std::make_unique<PulseNativeGuard>(a.baseline(),now());pulseCancelled_=cancelled;
        entryCheck_=[a,clock]{a.check(clock());};
    }
    void authorize(const GripLifecycleAuthorization& authorization,const std::function<std::int64_t()>& clock) {
        authorization.check(clock());if(binding_!=authorization.baseline().binding)throw ProtocolError("native lifecycle authorization binding mismatch");
        authorization.consume();lifecycle_=true;lifecycleSessionEnd_=now()+Time(75000000);
        for(const auto& phase:{gripBaselinePlan(),gripLifecyclePlan(authorization.baseline()),gripBaselinePlan()})
            for(const auto& r:phase)neutralReports_.push_back(binding_.layout.wrap(frame(r.command,r.payload)));
        entryCheck_=[authorization,clock]{authorization.check(clock());};
    }
    void authorize(const NeutralAuthorization& authorization,const std::function<std::int64_t()>& clock) {
        authorization.check(clock());if(binding_!=authorization.baseline().binding)throw ProtocolError("native authorization binding mismatch");
        for(const auto& phase:{snapshotPlan(authorization.baseline()),neutralPlan(authorization.baseline()),snapshotPlan(authorization.baseline())})
            for(const auto& request:phase)neutralReports_.push_back(binding_.layout.wrap(frame(request.command,request.payload)));
        entryCheck_=[authorization,clock]{authorization.check(clock());};
    }
    void authorize(const GripNeutralAuthorization& authorization,const std::function<std::int64_t()>& clock) {
        authorization.check(clock());if(binding_!=authorization.baseline().binding)throw ProtocolError("native grip authorization binding mismatch");
        for(const auto& phase:{gripBaselinePlan(),neutralPlan(authorization.baseline()),gripBaselinePlan()})
            for(const auto& request:phase)neutralReports_.push_back(binding_.layout.wrap(frame(request.command,request.payload)));
        entryCheck_=[authorization,clock]{authorization.check(clock());};
    }
    void authorize(const GripRestoreAuthorization& authorization,const std::function<std::int64_t()>& clock) {
        authorization.check(clock());if(binding_!=authorization.baseline().binding)throw ProtocolError("native restore-only authorization binding mismatch");
        for(const auto& phase:{gripBaselinePlan(),gripRestoreLeftPlan(authorization.baseline()),authorization.observeReplies()?std::vector<Request>{}:gripBaselinePlan()})
            for(const auto& request:phase)neutralReports_.push_back(binding_.layout.wrap(frame(request.command,request.payload)));
        entryCheck_=[authorization,clock]{authorization.check(clock());};
    }
#endif
    ~WindowsIo() override {closePending();if(handle_!=INVALID_HANDLE_VALUE&&!unresolved_)CloseHandle(handle_);}
#ifdef ASB_APEX6_LIVE_RUNNER
#ifdef ASB_APEX6_INTEGRATED
    bool dongleNeutral_=false;
    dongle::PulseSide donglePulse_=dongle::PulseSide::None;
    unsigned dongleWaves_=0;
    void authorizeDongleNeutral(const GripBaseline& baseline,
        const std::function<bool()>& cancelled,const std::function<void()>& entryCheck,live::ReplyBoundary boundary,dongle::PulseSide side=dongle::PulseSide::None) {
        try {
        require(gripReports_.size()==30&&gripIndex_==30&&!stopped_,"dongle requires two completed query sequences and UID boundaries on this handle");
        gripReports_.clear();gripIndex_=0;
        authorizeIntegrated(baseline,{1,1,false},cancelled,entryCheck);
        live_=std::make_unique<live::NativeGuard>(baseline,live::Policy{1,1,false},now(),boundary);
        dongleNeutral_=true;
        require(side==dongle::PulseSide::None||(dongle::validPulseSide(side)&&boundary==live::ReplyBoundary::DongleUidDiagnostic),"dongle pulse requires valid side and UID boundaries");
        donglePulse_=side;
        }catch(...){stopped_=true;throw;}
    }
    void authorizeIntegrated(const GripBaseline& baseline,live::Policy policy,
        const std::function<bool()>& cancelled,const std::function<void()>& entryCheck) {
        require(binding_==baseline.binding&&baseline.physicalOrigin&&bool(cancelled)&&bool(entryCheck),"integrated binding/controls missing");
        entryCheck();
        live_=std::make_unique<live::NativeGuard>(baseline,policy,now());
        liveCancelled_=cancelled;liveEntryCheck_=entryCheck;timings_.resize(4096);
    }
    void authorizeDongleLive(const GripBaseline& baseline,live::Policy policy,
        const std::function<bool()>& cancelled,const std::function<void()>& entryCheck) {
        try {
            require(gripReports_.size()==30&&gripIndex_==30&&!stopped_,"dongle live requires completed startup boundaries on this handle");
            gripReports_.clear();gripIndex_=0;
            authorizeIntegrated(baseline,policy,cancelled,entryCheck);
            live_=std::make_unique<live::NativeGuard>(baseline,policy,now(),live::ReplyBoundary::DongleLiveDiagnostic);
        }catch(...){stopped_=true;throw;}
    }
#endif
    void authorizeLive(const live::Authorization& a,const std::function<std::int64_t()>& clock,const std::function<bool()>& cancelled) {
        a.check(clock());require(binding_==a.baseline().binding&&bool(cancelled),"native live binding/cancellation mismatch");
        a.consume();live_=std::make_unique<live::NativeGuard>(a.baseline(),a.policy(),now());liveCancelled_=cancelled;
        liveEntryCheck_=[a,clock]{a.check(clock());};timings_.resize(131072);
    }
    void stopLive(Time requested)override{
        if(!liveReady())throw ProtocolError("native live failed");
        try {
            // liveReady advances the guard clock. The caller's stop intent was
            // sampled earlier; admit the transition at the native clock now.
            const auto admitted=now();
            require(requested<=admitted,"native live stop timestamp is in the future");
            live_->requestStop(admitted);
        }catch(...){stopped_=true;throw;}
    }
    void drainLiveTimings(const std::function<void(const NativeTiming&)>& sink)override{
        try{for(const auto& timing:timings())sink(timing);timingCount_=0;}
        catch(...){stopped_=true;throw;}
    }
#endif
    Time now() const override{return hooks_.clock();}
    bool physical() const override{return true;}
    const Binding& binding() const override{return binding_;}
    bool inputOnly() const override{return inputOnly_;}
    bool ram5Only() const override{return ram5Only_;}
    void restrictRam5(){if(binding_.access!=AccessMode::Shared)throw ProtocolError("RAM5 diagnostic requires shared access");ram5Only_=true;}
    void restrictGripBaseline() {
        if(binding_.access!=AccessMode::Shared)throw ProtocolError("grip baseline requires shared access");
        for(const auto& r:gripBaselinePlan())gripReports_.push_back(binding_.layout.wrap(frame(r.command,r.payload)));
    }
#ifdef ASB_APEX6_INTEGRATED
    void restrictDongleBaselines() {
        require(binding_.access==AccessMode::Shared&&gripReports_.empty(),"invalid dongle baseline binding");
        for(unsigned i=0;i<2;++i){
            for(const auto& r:gripBaselinePlan())gripReports_.push_back(binding_.layout.wrap(frame(r.command,r.payload)));
            gripReports_.push_back(binding_.layout.wrap(frame(4)));
        }
    }
#endif
    std::span<const NativeTiming> timings() const override{return {timings_.data(),timingCount_};}
    bool timingComplete() const override{return !timingOverflow_;}
    bool finish() override {stopped_=true;closePending(true);return !unresolved_;}
    bool stillSameDevice() override {
        if(queryCancelled_&&queryCancelled_())return false;
        DEVINST node{};auto id=wide(binding_.instance);
        if(CM_Locate_DevNodeW(&node,id.data(),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS)return false;
        ULONG status=0,problem=0;
        return CM_Get_DevNode_Status(&status,&problem,node,0)==CR_SUCCESS && problem==0 && (status&DN_STARTED);
    }
    IoResult read(Time deadline,bool poll) override {
#ifdef ASB_APEX6_LIVE_RUNNER
        if(live_){if(!liveReady())return {Completion::Failed,{},0,ERROR_OPERATION_ABORTED};deadline=live_->deadline(deadline);}
#endif
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        if(pulse_) {if(!pulseReady())return {Completion::Failed,{},0,ERROR_OPERATION_ABORTED};deadline=pulse_->deadline(deadline);}
        if(lifecycle_) {
            deadline=std::min(deadline,lifecycleSessionEnd_);
            if(neutralIndex_>14&&neutralIndex_<=18)deadline=std::min(deadline,lifecycleActiveEnd_);
        }
#endif
        if(stopped_)return {Completion::Failed,{},0,ERROR_OPERATION_ABORTED};
        if(now()>=deadline){stopped_=true;return {Completion::Timeout,{},0,ERROR_TIMEOUT};}
        // Keep the empty poll's pending read alive for the subsequent response.
        // Do not cancel it between drain and write, which could lose the reply.
        if(!read_) {
            if(!timingRoom())return {Completion::Failed,{},0,ERROR_BUFFER_OVERFLOW};
            read_=std::make_unique<Operation>(binding_.layout.inputLength);
            read_->id=++operationId_;stamp("read_submit",*read_);
            DWORD e=0;
#ifdef ASB_APEX6_NEUTRAL_RUNNER
            if(pulse_&&(!pulseReady()||now()>=deadline)){stopped_=true;read_.reset();return {Completion::Failed,{},0,ERROR_OPERATION_ABORTED};}
#endif
#ifdef ASB_APEX6_LIVE_RUNNER
            if(live_&&(!liveReady()||now()>=deadline)){stopped_=true;read_.reset();return {Completion::Failed,{},0,ERROR_OPERATION_ABORTED};}
#endif
            const auto submitted=hooks_.submit(true,handle_,read_->buffer.data(),static_cast<DWORD>(read_->buffer.size()),&read_->overlapped,e);
            stamp("read_submit_return",*read_,0,e);
            if(!submitted) {
                if(e!=ERROR_IO_PENDING){read_.reset();stopped_=true;return {Completion::Failed,{},0,e};}
            }
        }
        auto result=complete(read_,deadline,poll,true);
#ifdef ASB_APEX6_LIVE_RUNNER
        if(live_){try{live_->afterRead(result,now());if(!liveReady())throw ProtocolError("native live cancellation");}catch(...){stopped_=true;result.status=Completion::Failed;}}
#endif
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        if(pulse_) {try{pulse_->afterRead(result,now());}catch(...){stopped_=true;}}
#endif
        return result;
    }
    IoResult write(std::span<const std::uint8_t> wire,Time deadline) override {
        if(inputOnly_){stopped_=true;return {Completion::Failed,{},0,ERROR_ACCESS_DENIED};}
#ifdef ASB_APEX6_INTEGRATED
        if(dongleNeutral_&&wire.size()>3&&wire[3]==0x57) {
            if(donglePulse_!=dongle::PulseSide::None?!dongle::allowedPulseWave(wire,dongleWaves_,donglePulse_):(dongleWaves_>=11||Bytes(wire.begin(),wire.end())!=binding_.layout.wrap(gripWaveform({})))) {
                stopped_=true;return {Completion::Failed,{},0,ERROR_ACCESS_DENIED};
            }
            ++dongleWaves_;
        }
#endif
        bool allowed=queryOnly(wire,binding_.layout);
#ifdef ASB_APEX6_LIVE_RUNNER
        if(live_){allowed=liveReady();deadline=live_->deadline(deadline);}
#endif
        if(!gripReports_.empty()) {
            allowed=allowed&&gripIndex_<gripReports_.size()&&Bytes(wire.begin(),wire.end())==gripReports_[gripIndex_];
            if(allowed)++gripIndex_;
        }
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        if(pulse_) {allowed=pulseReady();deadline=pulse_->deadline(deadline);}
        if(!neutralReports_.empty()) {
            allowed=neutralIndex_<neutralReports_.size()&&Bytes(wire.begin(),wire.end())==neutralReports_[neutralIndex_];
            if(lifecycle_) {
                deadline=std::min(deadline,lifecycleSessionEnd_);
                if(now()>=lifecycleSessionEnd_)allowed=false;
                if(neutralIndex_==14)lifecycleActiveEnd_=now()+Time(5000000);
                if(neutralIndex_>=14&&neutralIndex_<18) {
                    deadline=std::min(deadline,lifecycleActiveEnd_);
                    if(now()>=lifecycleActiveEnd_)allowed=false;
                }
            }
            if(allowed&&wire[3]==0x53&&!entryChecked_) {
                try{entryCheck_();entryChecked_=true;}catch(...){allowed=false;}
            }
            if(allowed)++neutralIndex_;
        }
#endif
        if(ram5Only_) {
            allowed=!ram5Attempted_&&Bytes(wire.begin(),wire.end())==binding_.layout.wrap(frame(0xa3,Bytes{1,5}));
            ram5Attempted_=true;
        }
        if(stopped_||!allowed) {stopped_=true;return {Completion::Failed,{},0,ERROR_ACCESS_DENIED};}
        if(now()>=deadline){stopped_=true;return {Completion::Timeout,{},0,ERROR_TIMEOUT};}
        if(!timingRoom())return {Completion::Failed,{},0,ERROR_BUFFER_OVERFLOW};
        auto op=std::make_unique<Operation>(wire.size());std::copy(wire.begin(),wire.end(),op->buffer.begin());
        op->id=++operationId_;stamp("write_submit",*op);
        DWORD e=0;
        Time submittedAt=now();
#ifdef ASB_APEX6_LIVE_RUNNER
        if(live_){try{
            if(!liveReady())throw ProtocolError("native live failed");
            if(live_->entryNext())liveEntryCheck_();
            if(!liveReady())throw ProtocolError("native live cancelled before submission");
            submittedAt=now();require(submittedAt<deadline,"live caller deadline reached");
            live_->beforeWrite(wire,submittedAt);deadline=live_->deadline(deadline);
        }catch(...){stopped_=true;return {Completion::Failed,{},0,ERROR_ACCESS_DENIED};}}
#endif
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        if(pulse_) {
            try {
                if(!pulseReady())throw ProtocolError("pulse cancelled or failed");
                if(pulse_->nextIndex()==14)entryCheck_();
                if(!pulseReady())throw ProtocolError("pulse cancelled before submission");
                submittedAt=now();require(submittedAt<deadline,"pulse caller deadline reached before submission");
                pulse_->beforeWrite(wire,submittedAt);deadline=pulse_->deadline(deadline);
            }catch(...){stopped_=true;return {Completion::Failed,{},0,ERROR_ACCESS_DENIED};}
        }
#endif
        const auto submitted=hooks_.submit(false,handle_,op->buffer.data(),static_cast<DWORD>(op->buffer.size()),&op->overlapped,e);
        stamp("write_submit_return",*op,0,e);
        if(!submitted) {
            if(e!=ERROR_IO_PENDING){stopped_=true;return {Completion::Failed,{},0,e};}
        }
        auto result=complete(op,deadline,false,false);const auto completedAt=now();
        result.submittedAt=submittedAt;result.completedAt=completedAt;
#ifdef ASB_APEX6_LIVE_RUNNER
        if(live_){try{live_->afterWrite(result,submittedAt,completedAt);if(!liveReady())throw ProtocolError("native live cancellation");}catch(...){stopped_=true;result.status=Completion::Failed;}}
#endif
#ifdef ASB_APEX6_NEUTRAL_RUNNER
        if(pulse_) {
            try{pulse_->afterWrite(result,submittedAt,completedAt);}catch(...){stopped_=true;if(result.status==Completion::Complete)result.status=Completion::Failed;}
        }
#endif
        return result;
    }
    // Called by the factory's custom worker lifetime through destructor below.
    void closePending(bool boundedWait=false) noexcept {
        if(read_) {
            stamp("read_cancel_requested",*read_);
            hooks_.cancel(handle_,&read_->overlapped);DWORD count=0,error=0;
            if(boundedWait) {
                const auto before=now(),deadline=before+Time(50000);
                stampWait("cancel_wait_begin",*read_,before,deadline,50,0,0);
                const auto result=hooks_.wait(read_->event.value,50);
                const auto waitError=result==WAIT_FAILED?GetLastError():0;
                stampWait("cancel_wait_return",*read_,now(),deadline,50,result,waitError);
            }
            const auto ok=hooks_.result(handle_,&read_->overlapped,count,error);
            stamp("read_cancel_completion_observed",*read_,count,error);
            if(!ok&&unresolvedCompletion(error)){unresolved_=true;read_.release();}
            else read_.reset();
        }
    }
private:
    std::function<bool()> queryCancelled_;
public:
    void setQueryCancellation(const std::function<bool()>& cancelled){queryCancelled_=cancelled;}
private:
#ifdef ASB_APEX6_LIVE_RUNNER
    bool liveReady(){if(stopped_)return false;try{require(!liveCancelled_(),"live cancelled");live_->checkTime(now());return true;}catch(...){stopped_=true;return false;}}
    std::unique_ptr<live::NativeGuard> live_;
    std::function<bool()> liveCancelled_;
    std::function<void()> liveEntryCheck_;
#endif
#ifdef ASB_APEX6_NEUTRAL_RUNNER
    bool pulseReady() {
        if(stopped_)return false;
        try{require(!pulseCancelled_(),"pulse cancelled");pulse_->checkTime(now());return true;}catch(...){stopped_=true;return false;}
    }
#endif
    bool timingRoom(){if(timingCount_+32>timings_.size()){timingOverflow_=true;stopped_=true;return false;}return true;}
    void stamp(const char* event,const Operation& op,DWORD count=0,DWORD error=0) noexcept {
        if(timingCount_==timings_.size()){timingOverflow_=true;stopped_=true;return;}
        auto& t=timings_[timingCount_++];t={now(),event,op.id,count,error};
        t.rawSize=std::min<std::size_t>({count,op.buffer.size(),t.raw.size()});
        std::copy_n(op.buffer.begin(),t.rawSize,t.raw.begin());
    }
    void stampWait(const char* event,const Operation& op,Time observed,Time deadline,DWORD milliseconds,DWORD result,DWORD error) noexcept {
        if(timingCount_==timings_.size()){timingOverflow_=true;stopped_=true;return;}
        auto& t=timings_[timingCount_++];t={observed,event,op.id,0,error};
        t.waitMetadata=true;t.deadline=deadline;t.requestedWaitMs=milliseconds;t.waitResult=result;
    }
    IoResult complete(std::unique_ptr<Operation>& op,Time deadline,bool poll,bool reading) {
        DWORD wait=WAIT_FAILED,waitError=0;auto previous=now();
        // A millisecond wait may wake before our monotonic deadline. Re-wait
        // the SAME OVERLAPPED operation, never cancel/re-submit or extend time.
        // Bound pathological early wakes even if the test/system clock stalls.
        for(unsigned wake=0;wake<8;++wake) {
            const auto before=now();
            if(before<previous){wait=WAIT_FAILED;waitError=ERROR_INVALID_DATA;stamp("wait_clock_reversed",*op,0,waitError);break;}
            if(!poll&&before>=deadline){wait=WAIT_TIMEOUT;waitError=ERROR_TIMEOUT;stamp("wait_deadline_reached",*op,0,waitError);break;}
            if(!timingRoom()){wait=WAIT_FAILED;waitError=ERROR_BUFFER_OVERFLOW;break;}
            const auto milliseconds=poll?0:waitMs(deadline,before);
            // Empty polls intentionally have no wait records: retaining their
            // pending read must not exhaust evidence during quiet listeners.
            if(!poll)stampWait(reading?"read_wait_begin":"write_wait_begin",*op,before,deadline,milliseconds,0,0);
            wait=hooks_.wait(op->event.value,milliseconds);
            waitError=wait==WAIT_FAILED?GetLastError():wait==WAIT_TIMEOUT?ERROR_TIMEOUT:wait==WAIT_OBJECT_0?0:ERROR_INVALID_DATA;
            const auto after=now();
            if(!poll||wait!=WAIT_TIMEOUT)stampWait(reading?"read_wait_return":"write_wait_return",*op,after,deadline,milliseconds,wait,waitError);
            if(after<before){wait=WAIT_FAILED;waitError=ERROR_INVALID_DATA;stamp("wait_clock_reversed",*op,0,waitError);break;}
            previous=after;
            if(poll&&wait==WAIT_TIMEOUT)return {Completion::Idle,{}};
            if(wait!=WAIT_TIMEOUT||after>=deadline)break;
            stamp("wait_early_wake",*op);
            if(wake==7){wait=WAIT_FAILED;waitError=ERROR_RETRY;stamp("wait_early_wake_limit",*op,0,waitError);}
        }
        if(wait!=WAIT_OBJECT_0) {
            stopped_=true;
            const auto failure=wait==WAIT_TIMEOUT?Completion::Timeout:Completion::Failed;
            stamp(reading?"read_cancel_requested":"write_cancel_requested",*op);
            hooks_.cancel(handle_,&op->overlapped);DWORD transferred=0,e=0;
            const auto ok=hooks_.result(handle_,&op->overlapped,transferred,e);
            stamp(reading?"read_cancel_completion_observed":"write_cancel_completion_observed",*op,transferred,e);
            if(!ok) {
                if(unresolvedCompletion(e)) {unresolved_=true;op.release();return {Completion::Unresolved,{},0,waitError};}
                op.reset();
                // Preserve the wait failure; cancellation status is separately
                // logged. A device/completion failure is not ordinary silence.
                if(failure==Completion::Timeout&&e!=ERROR_OPERATION_ABORTED)return {Completion::Failed,{},0,e};
                return {failure,{},0,waitError};
            }
            Bytes data;if(reading)data.assign(op->buffer.begin(),op->buffer.begin()+std::min<std::size_t>(transferred,op->buffer.size()));
            op.reset();return {failure,std::move(data),transferred,waitError};
        }
        DWORD transferred=0,e=0;
        const auto ok=hooks_.result(handle_,&op->overlapped,transferred,e);
        stamp(reading?"read_completion_observed":"write_completion_observed",*op,transferred,e);
        if(!ok) {
            stopped_=true;
            if(unresolvedCompletion(e)){unresolved_=true;op.release();return {Completion::Unresolved,{},0,e};}
            op.reset();return {Completion::Failed,{},0,e};
        }
        Bytes data;if(reading)data.assign(op->buffer.begin(),op->buffer.begin()+std::min<std::size_t>(transferred,op->buffer.size()));
        if(transferred!=op->buffer.size())stopped_=true;
        op.reset();return {Completion::Complete,std::move(data),transferred,0};
    }
    Binding binding_;HANDLE handle_=INVALID_HANDLE_VALUE;std::unique_ptr<Operation> read_;
    WindowsIoHooks hooks_=realCalls();
    bool stopped_=false,unresolved_=false;
    bool inputOnly_=false,timingOverflow_=false;std::uint64_t operationId_=0;
    bool ram5Only_=false,ram5Attempted_=false;
    std::vector<Bytes> gripReports_;std::size_t gripIndex_=0;
    std::vector<NativeTiming> timings_=std::vector<NativeTiming>(2048);std::size_t timingCount_=0;
#ifdef ASB_APEX6_NEUTRAL_RUNNER
    std::vector<Bytes> neutralReports_;std::size_t neutralIndex_=0;
    std::function<void()> entryCheck_;bool entryChecked_=false;
    bool lifecycle_=false;Time lifecycleSessionEnd_{},lifecycleActiveEnd_{};
    std::unique_ptr<PulseNativeGuard> pulse_;std::function<bool()> pulseCancelled_;
#endif
};
}
std::string utf8(const std::wstring& text) {
    if(text.empty())return {};int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);require(n>0,"UTF8 conversion failed");
    std::string out(n,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);return out;
}
std::wstring wide(const std::string& text) {
    if(text.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);require(n>0,"UTF16 conversion failed");
    std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n);return out;
}
std::vector<HidDeviceInfo> vendorInterfaces() {
    std::string error;auto devices=platform::enumerateHidDevices(error);if(!error.empty())throw ProtocolError(error);
    std::erase_if(devices,[](const auto& d){return d.vendorId!=0x37d7 || (d.productId!=0x2502&&d.productId!=0x2401) || d.usagePage!=0xffa0;});return devices;
}
Binding inspectInterface(const HidDeviceInfo& info) {
    require(info.vendorId==0x37d7&&(info.productId==0x2502||info.productId==0x2401)&&info.usagePage==0xffa0,"wrong vendor interface");
    require(!info.instanceId.empty()&&!info.containerId.empty(),"missing exact device identity");
    Handle handle;handle.value=CreateFileW(info.path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);require(handle.value!=INVALID_HANDLE_VALUE,"metadata open failed");
    Binding b{utf8(info.path),utf8(info.instanceId),utf8(info.containerId),{},{}};b.layout=describe(handle.value,b.descriptorSignature);return b;
}
std::unique_ptr<WindowsTransport> openQueryTransport(const Binding& b){return std::make_unique<WindowsIo>(b);}
#ifdef ASB_APEX6_LIVE_RUNNER
std::unique_ptr<WindowsTransport> openLiveTransport(const live::Authorization& a,const std::function<std::int64_t()>& clock,const std::function<bool()>& cancelled){
    a.check(clock());require(!cancelled(),"live cancelled before open");auto io=std::make_unique<WindowsIo>(a.baseline().binding,false,true);io->authorizeLive(a,clock,cancelled);return io;
}
#ifdef ASB_APEX6_INTEGRATED
std::unique_ptr<WindowsTransport> openIntegratedBaselineTransport(const Binding& b,const std::function<bool()>& cancelled) {
    require(bool(cancelled)&&!cancelled(),"Apex6 baseline cancelled");
    try {auto io=std::make_unique<WindowsIo>(b,false,true);io->restrictGripBaseline();io->setQueryCancellation(cancelled);return io;}
    catch(const std::exception& e){throw ProtocolError(std::string("Cannot acquire exclusive Apex6 vendor access. Close Flydigi Space Station, stop its service and close competing writers: ")+e.what());}
}
std::unique_ptr<WindowsTransport> openDongleBaselineTransport(const Binding& b,const std::function<bool()>& cancelled) {
    require(bool(cancelled)&&!cancelled(),"dongle baseline cancelled");
    auto io=std::make_unique<WindowsIo>(b,false,true);
    io->restrictDongleBaselines();io->setQueryCancellation(cancelled);return io;
}
std::unique_ptr<WindowsTransport> makeDongleBaselineIoForTest(const Binding& b,HANDLE h,WindowsIoHooks hooks) {
    auto io=std::make_unique<WindowsIo>(b,h,std::move(hooks));io->restrictDongleBaselines();return io;
}
void promoteDongleNeutralTransport(WindowsTransport& transport,const GripBaseline& baseline,
    const std::function<bool()>& cancelled,const std::function<void()>& entryCheck,live::ReplyBoundary boundary) {
    auto* io=dynamic_cast<WindowsIo*>(&transport);require(io!=nullptr,"wrong dongle native transport");
    io->authorizeDongleNeutral(baseline,cancelled,entryCheck,boundary);
}
void promoteDonglePulseTransport(WindowsTransport& transport,const GripBaseline& baseline,
    const std::function<bool()>& cancelled,const std::function<void()>& entryCheck,dongle::PulseSide side) {
    auto* io=dynamic_cast<WindowsIo*>(&transport);require(io!=nullptr,"wrong dongle native transport");
    // Invalid selections also latch the transport through the authorization gate.
    io->authorizeDongleNeutral(baseline,cancelled,entryCheck,live::ReplyBoundary::DongleUidDiagnostic,side==dongle::PulseSide::None?static_cast<dongle::PulseSide>(-1):side);
}
std::unique_ptr<WindowsTransport> openIntegratedTransport(const GripBaseline& b,live::Policy p,
    const std::function<bool()>& cancelled,const std::function<void()>& entryCheck) {
    require(!cancelled(),"integrated session cancelled");
    auto io=std::make_unique<WindowsIo>(b.binding,false,true);
    io->authorizeIntegrated(b,p,cancelled,entryCheck);return io;
}
void promoteDongleLiveTransport(WindowsTransport& transport,const GripBaseline& baseline,live::Policy policy,
    const std::function<bool()>& cancelled,const std::function<void()>& entryCheck) {
    auto* io=dynamic_cast<WindowsIo*>(&transport);require(io!=nullptr,"wrong dongle live native transport");
    io->authorizeDongleLive(baseline,policy,cancelled,entryCheck);
}
#endif
std::unique_ptr<WindowsTransport> makeLiveIoForTest(const live::Authorization& a,HANDLE h,WindowsIoHooks hooks,const std::function<std::int64_t()>& clock,const std::function<bool()>& cancelled){auto io=std::make_unique<WindowsIo>(a.baseline().binding,h,std::move(hooks));io->authorizeLive(a,clock,cancelled);return io;}
#endif
std::unique_ptr<WindowsTransport> openGripBaselineTransport(const Binding& b) {
    if(b.access!=AccessMode::Shared)throw ProtocolError("grip baseline requires shared access");
    auto io=std::make_unique<WindowsIo>(b);io->restrictGripBaseline();return io;
}
std::unique_ptr<WindowsTransport> openInputListener(const Binding& b){return std::make_unique<WindowsIo>(b,true);}
std::unique_ptr<WindowsTransport> openRam5Diagnostic(const Binding& b) {
    if(b.access!=AccessMode::Shared)throw ProtocolError("RAM5 diagnostic requires shared access");
    auto io=std::make_unique<WindowsIo>(b);io->restrictRam5();return io;
}
Ram5Result examineRam5(WindowsTransport& io,Trace& trace,const std::function<void()>& idle) {
    Ram5Result r;
    try {
        if(!io.ram5Only()||io.inputOnly()||io.binding().access!=AccessMode::Shared)throw ProtocolError("requires one-shot shared RAM5 transport");
        const auto capture=[&](Time deadline,bool after) {
            while(io.now()<deadline&&r.before+r.after<64) {
                if(!trace.healthy())throw ProtocolError("diagnostic trace failed");
                const auto input=io.read(deadline,true);
                if(input.status==Completion::Idle){idle();continue;}
                trace.record(io.now(),after?"ram5_post_write_input":"ram5_pre_write_input",input.bytes,0,input.error);
                if(input.status!=Completion::Complete||input.transferred!=io.binding().layout.inputLength||input.bytes.size()!=io.binding().layout.inputLength)
                    throw ProtocolError("diagnostic read failed/short (Win32 "+std::to_string(input.error)+")");
                if(after)++r.after;else ++r.before;
                bool candidate=false;
                try {const auto payload=replyPayload(io.binding().layout.unwrap(input.bytes),0xa3,10,true);
                    if(payload){ramInfo(5,*payload);candidate=true;}}
                catch(const ProtocolError&){} // Raw data retained; never promoted to acquisition.
                if(after&&candidate)++r.candidates;else ++r.unexpected;
            }
        };
        capture(io.now()+Time(250000),false);
        if(r.before==64)throw ProtocolError("pre-listen report limit; no query sent");
        if(!trace.healthy())throw ProtocolError("diagnostic trace failed before query");
        const auto wire=io.binding().layout.wrap(frame(0xa3,Bytes{1,5}));
        const auto deadline=io.now()+Time(600000);
        trace.record(io.now(),"ram5_single_write_intent",wire);
        if(!trace.healthy())throw ProtocolError("diagnostic intent trace failed");
        ++r.writes;const auto sent=io.write(wire,deadline);
        trace.record(io.now(),"ram5_single_write_return",{},sent.transferred,sent.error);
        if(sent.status!=Completion::Complete||sent.transferred!=wire.size()||io.now()>=deadline)throw ProtocolError("diagnostic write failed/short/late; no retry");
        capture(deadline,true);
        r.stop=r.before+r.after==64?"report_limit":"time_limit";
        r.complete=trace.healthy();if(!r.complete)r.error="diagnostic trace failed";
    }catch(const std::exception& e){r.error=e.what();r.stop="error";}
    if(!io.finish()){r.complete=false;r.error+=" unresolved cancellation";}
    if(!io.timingComplete()){r.complete=false;r.error+=" native timing incomplete";}
    return r;
}
ListenResult listenInput(WindowsTransport& io,Trace& trace,const std::function<void()>& idle) {
    ListenResult r;const auto start=io.now(),deadline=start+Time(3000000);
    try {
        if(!io.inputOnly()||io.binding().access!=AccessMode::Shared)throw ProtocolError("listener requires input-only shared transport");
        while(r.reports<64&&io.now()<deadline) {
            if(!trace.healthy())throw ProtocolError("listener trace failed");
            const auto result=io.read(deadline,true);
            if(result.status==Completion::Idle){idle();continue;}
            if(result.status!=Completion::Complete)throw ProtocolError("listener read failed (Win32 "+std::to_string(result.error)+")");
            ++r.reports;trace.record(io.now(),"unsolicited_input",result.bytes,r.reports,result.transferred);
            if(result.transferred!=io.binding().layout.inputLength||result.bytes.size()!=io.binding().layout.inputLength)throw ProtocolError("listener short report");
        }
        r.stop=r.reports==64?"report_limit":"time_limit";r.complete=trace.healthy();
    }catch(const std::exception& e){r.stop="error";r.error=e.what();}
    r.elapsed=io.now()-start;
    if(!io.finish()){r.complete=false;r.error+=" unresolved input cancellation; retained until worker exit";}
    if(!io.timingComplete()){r.complete=false;r.error+=" native timing incomplete";}
    return r;
}
std::uint32_t queryShareFlags(AccessMode mode) {
    if(mode==AccessMode::Exclusive)return 0;if(mode==AccessMode::Shared)return FILE_SHARE_READ|FILE_SHARE_WRITE;
    throw ProtocolError("physical transport requires exclusive or shared access");
}
#ifdef ASB_APEX6_NEUTRAL_RUNNER
std::unique_ptr<WindowsTransport> openNeutralTransport(const NeutralAuthorization& a,const std::function<std::int64_t()>& clock) {
    a.check(clock());auto io=std::make_unique<WindowsIo>(a.baseline().binding);io->authorize(a,clock);return io;
}
std::unique_ptr<WindowsTransport> openGripPulseTransport(const GripPulseAuthorization& a,const std::function<std::int64_t()>& clock,const std::function<bool()>& cancelled) {
    a.check(clock());require(!cancelled(),"pulse cancelled before open");auto io=std::make_unique<WindowsIo>(a.baseline().binding);io->authorize(a,clock,cancelled);return io;
}
std::unique_ptr<WindowsTransport> makeGripPulseIoForTest(const GripPulseAuthorization& a,HANDLE h,WindowsIoHooks hooks,const std::function<std::int64_t()>& clock,const std::function<bool()>& cancelled) {
    auto io=std::make_unique<WindowsIo>(a.baseline().binding,h,std::move(hooks));io->authorize(a,clock,cancelled);return io;
}
std::unique_ptr<WindowsTransport> openGripLifecycleTransport(const GripLifecycleAuthorization& a,const std::function<std::int64_t()>& clock) {
    a.check(clock());auto io=std::make_unique<WindowsIo>(a.baseline().binding);io->authorize(a,clock);return io;
}
std::unique_ptr<WindowsTransport> makeGripLifecycleIoForTest(const GripLifecycleAuthorization& a,HANDLE h,WindowsIoHooks hooks,const std::function<std::int64_t()>& clock) {
    auto io=std::make_unique<WindowsIo>(a.baseline().binding,h,std::move(hooks));io->authorize(a,clock);return io;
}
std::unique_ptr<Io> makeNeutralIoForTest(const NeutralAuthorization& a,HANDLE h,WindowsIoHooks hooks,const std::function<std::int64_t()>& clock) {
    auto io=std::make_unique<WindowsIo>(a.baseline().binding,h,std::move(hooks));io->authorize(a,clock);return io;
}
std::unique_ptr<WindowsTransport> openGripNeutralTransport(const GripNeutralAuthorization& a,const std::function<std::int64_t()>& clock) {
    a.check(clock());auto io=std::make_unique<WindowsIo>(a.baseline().binding);io->authorize(a,clock);return io;
}
std::unique_ptr<Io> makeGripNeutralIoForTest(const GripNeutralAuthorization& a,HANDLE h,WindowsIoHooks hooks,const std::function<std::int64_t()>& clock) {
    auto io=std::make_unique<WindowsIo>(a.baseline().binding,h,std::move(hooks));io->authorize(a,clock);return io;
}
std::unique_ptr<WindowsTransport> openGripRestoreTransport(const GripRestoreAuthorization& a,const std::function<std::int64_t()>& clock) {
    a.check(clock());auto io=std::make_unique<WindowsIo>(a.baseline().binding);io->authorize(a,clock);return io;
}
std::unique_ptr<WindowsTransport> makeGripRestoreIoForTest(const GripRestoreAuthorization& a,HANDLE h,WindowsIoHooks hooks,const std::function<std::int64_t()>& clock) {
    auto io=std::make_unique<WindowsIo>(a.baseline().binding,h,std::move(hooks));io->authorize(a,clock);return io;
}
#endif
std::vector<AccessProbeResult> probeInterfaceAccessForTest(const Binding& b,const WindowsAccessHooks& calls) {
    if(b.path.empty()||b.instance.empty()||b.container.empty())throw ProtocolError("access probe requires exact binding");
    b.layout.validate();const auto path=wide(b.path);
    std::vector<AccessProbeResult> results{
        {"metadata_shared_rw",0,FILE_SHARE_READ|FILE_SHARE_WRITE},
        {"read_exclusive",GENERIC_READ,0},
        {"read_share_read",GENERIC_READ,FILE_SHARE_READ},
        {"read_share_rw",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE},
        {"read_write_exclusive",GENERIC_READ|GENERIC_WRITE,0},
        {"read_write_share_read",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ},
        {"read_write_share_rw",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE}
    };
    for(std::size_t i=0;i<results.size();++i) {
        auto& r=results[i];DWORD error=0;
        const auto handle=calls.open(path,r.access,r.share,error);
        r.opened=handle!=INVALID_HANDLE_VALUE&&handle!=nullptr;r.openError=r.opened?0:error;
        if(r.opened) {
            r.closed=calls.close(handle,error)!=FALSE;r.closeError=r.closed?0:error;
            // Unknown handle lifetime: stop. The supervisor owns worker teardown.
            if(!r.closed){results.resize(i+1);break;}
        } else if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED) {
            // Removal/unexpected errors are not grounds to try another case.
            results.resize(i+1);break;
        }
    }
    return results;
}
std::vector<AccessProbeResult> probeInterfaceAccess(const Binding& b) {
    return probeInterfaceAccessForTest(b,{
        [](const std::wstring& path,DWORD access,DWORD share,DWORD& error){
            auto h=CreateFileW(path.c_str(),access,share,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            error=h==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;return h;
        },
        [](HANDLE h,DWORD& error){auto ok=CloseHandle(h);error=ok?ERROR_SUCCESS:GetLastError();return ok;}
    });
}
std::unique_ptr<WindowsTransport> makeQueryIoForTest(const Binding& b,HANDLE h,WindowsIoHooks hooks){return std::make_unique<WindowsIo>(b,h,std::move(hooks));}
std::unique_ptr<WindowsTransport> makeGripBaselineIoForTest(const Binding& b,HANDLE h,WindowsIoHooks hooks){auto io=std::make_unique<WindowsIo>(b,h,std::move(hooks));io->restrictGripBaseline();return io;}
std::unique_ptr<WindowsTransport> makeInputListenerForTest(const Binding& b,HANDLE h,WindowsIoHooks hooks){return std::make_unique<WindowsIo>(b,h,std::move(hooks),true);}
std::unique_ptr<WindowsTransport> makeRam5DiagnosticForTest(const Binding& b,HANDLE h,WindowsIoHooks hooks){auto io=std::make_unique<WindowsIo>(b,h,std::move(hooks));io->restrictRam5();return io;}
std::string sha256(std::span<const std::uint8_t> bytes) {
    struct Hash {BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE value=nullptr;~Hash(){if(value)BCryptDestroyHash(value);if(alg)BCryptCloseAlgorithmProvider(alg,0);}} hash;
    require(BCryptOpenAlgorithmProvider(&hash.alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&BCryptCreateHash(hash.alg,&hash.value,nullptr,0,nullptr,0,0)>=0,"SHA256 initialization failed");
    require(bytes.size()<=0xffffffffu,"SHA256 input too large");
    require(BCryptHashData(hash.value,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)>=0,"SHA256 update failed");
    std::array<std::uint8_t,32> digest{};require(BCryptFinishHash(hash.value,digest.data(),static_cast<ULONG>(digest.size()),0)>=0,"SHA256 finish failed");return hex(digest);
}
std::string sha256File(const std::filesystem::path& path) {
    if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>64*1024*1024)throw ProtocolError("hash input must be a regular file <=64 MiB");
    std::ifstream in(path,std::ios::binary);Bytes bytes(static_cast<std::size_t>(std::filesystem::file_size(path)));
    if(!in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))||in.peek()!=EOF)throw ProtocolError("file changed or hash read failed");return sha256(bytes);
}
}
