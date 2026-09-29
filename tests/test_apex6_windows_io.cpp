#include "apex6/experiment/WindowsIoTestHooks.h"
#include "apex6_rehearsal_fixture.h"
#include "apex6/experiment/WindowsPulseWait.h"
#include <iostream>
#include <map>
#include <deque>
#include <algorithm>

using namespace asb::apex6;
using namespace asb::apex6::experiment;
namespace {
unsigned checks=0;
void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
struct FakeKernel {
    enum Mode {Immediate,Pending,Short,TimeoutCancelled,TimeoutLateSuccess,Unresolved,SubmitFailure,CompletionFailure,SignaledIncomplete,WaitFailure};
    Mode mode=Immediate;bool poll=false;unsigned submissions=0,cancels=0,reads=0,writes=0;Time clock{};Bytes reply;
    struct PendingData{void* buffer;DWORD size;};std::map<OVERLAPPED*,PendingData> operations;
    WindowsIoHooks hooks() {
        return {
            [this](bool read,HANDLE,void* p,DWORD n,OVERLAPPED* o,DWORD& e)->BOOL{++submissions;if(read){++reads;if(!reply.empty())std::copy_n(reply.begin(),std::min<std::size_t>(n,reply.size()),static_cast<std::uint8_t*>(p));}else ++writes;operations[o]={p,n};if(mode==SubmitFailure){e=ERROR_DEVICE_NOT_CONNECTED;return FALSE;}e=mode==Immediate?0:ERROR_IO_PENDING;return mode==Immediate?TRUE:FALSE;},
            [this](HANDLE,DWORD timeout)->DWORD{if(poll&&timeout==0)return WAIT_TIMEOUT;if(mode==WaitFailure){SetLastError(ERROR_INVALID_HANDLE);return WAIT_FAILED;}if(mode==TimeoutCancelled||mode==TimeoutLateSuccess||mode==Unresolved){clock+=Time(static_cast<Time::rep>(timeout)*1000);return WAIT_TIMEOUT;}return WAIT_OBJECT_0;},
            [this](HANDLE,OVERLAPPED* o,DWORD& n,DWORD& e)->BOOL{if(mode==Unresolved||mode==SignaledIncomplete){e=ERROR_IO_INCOMPLETE;return FALSE;}if(mode==TimeoutCancelled){e=ERROR_OPERATION_ABORTED;return FALSE;}if(mode==CompletionFailure||mode==WaitFailure){e=ERROR_DEVICE_NOT_CONNECTED;return FALSE;}n=operations.at(o).size-(mode==Short?1:0);e=0;return TRUE;},
            [this](HANDLE,OVERLAPPED*){++cancels;},[this]{return clock;}
        };
    }
};
struct ListenerTrace:Trace {
    unsigned reports=0;bool good=true;
    void record(Time,const std::string& event,std::span<const std::uint8_t> raw,std::uint64_t,std::uint64_t) override {
        check(event=="unsolicited_input"&&!raw.empty(),"listener attributed or dropped raw report");++reports;
    }
    bool healthy()const override{return good;}
};
struct DiagnosticTrace:Trace {
    bool good=true;std::string failOn;std::vector<Bytes> inputs;
    void record(Time,const std::string& event,std::span<const std::uint8_t> raw,std::uint64_t,std::uint64_t) override {
        if(event==failOn)good=false;
        if(event=="ram5_pre_write_input"||event=="ram5_post_write_input")inputs.emplace_back(raw.begin(),raw.end());
    }
    bool healthy()const override{return good;}
};
struct Ram5Kernel {
    Time clock{};unsigned writes=0;bool pre=false,failWrite=false,unresolved=false;
    OVERLAPPED* readOp=nullptr;HANDLE readEvent=nullptr;void* buffer=nullptr;
    std::deque<Bytes> inputs;Bytes sent;
    WindowsIoHooks hooks(){return {
        [this](bool read,HANDLE,void* p,DWORD n,OVERLAPPED* o,DWORD& e)->BOOL {
            if(read){readOp=o;readEvent=o->hEvent;buffer=p;}
            else {++writes;sent.assign(static_cast<std::uint8_t*>(p),static_cast<std::uint8_t*>(p)+n);if(failWrite){e=ERROR_DEVICE_NOT_CONNECTED;return FALSE;}}
            e=ERROR_IO_PENDING;return FALSE;
        },
        [this](HANDLE event,DWORD)->DWORD {return event!=readEvent||((pre||writes)&&!inputs.empty())?WAIT_OBJECT_0:WAIT_TIMEOUT;},
        [this](HANDLE,OVERLAPPED* o,DWORD& n,DWORD& e)->BOOL {
            if(o==readOp){if(inputs.empty()){e=unresolved?ERROR_IO_INCOMPLETE:ERROR_OPERATION_ABORTED;return FALSE;}
                n=static_cast<DWORD>(inputs.front().size());std::copy(inputs.front().begin(),inputs.front().end(),static_cast<std::uint8_t*>(buffer));inputs.pop_front();}
            else n=33;e=0;return TRUE;
        },[](HANDLE,OVERLAPPED*){},[this]{return clock;}
    };}
};
}
int main(){try {
    const auto binding=rehearsalFixture().binding;
    check(queryShareFlags(AccessMode::Exclusive)==0&&queryShareFlags(AccessMode::Shared)==3,"access flags/fallback");
    auto shared=binding;shared.access=AccessMode::Shared;
    {
        FakeKernel kernel;auto io=makeGripBaselineIoForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        for(const auto& r:gripBaselinePlan())check(io->write(shared.layout.wrap(frame(r.command,r.payload)),Time(600000)).status==Completion::Complete,"native grip plan refused");
        check(kernel.writes==14,"native grip count");
        check(io->write(shared.layout.wrap(frame(1)),Time(600000)).status==Completion::Failed&&kernel.writes==14,"native grip allowed repeat");
    }
    const auto gripPlan=gripBaselinePlan();
    for(unsigned index=0;index<gripPlan.size();++index) {
        FakeKernel kernel;auto io=makeGripBaselineIoForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        for(unsigned i=0;i<index;++i)io->write(shared.layout.wrap(frame(gripPlan[i].command,gripPlan[i].payload)),Time(600000));
        check(io->write(shared.layout.wrap(frame(0xa3,Bytes{1,5})),Time(600000)).status==Completion::Failed&&kernel.writes==index,"native grip allowed RAM5");
        const auto& r=gripPlan[index];check(io->write(shared.layout.wrap(frame(r.command,r.payload)),Time(600000)).status==Completion::Failed&&kernel.writes==index,"native grip refusal not latched");
    }
    const auto ram5Request=shared.layout.wrap(frame(0xa3,Bytes{1,5}));
    check(hex(ram5Request)=="005aa5a304010500000000000000000000000000000000000000000000000000ad","RAM5 request differs from reviewed wire");
    for(unsigned index=0;index<ram5Request.size();++index) {
        FakeKernel kernel;auto io=makeRam5DiagnosticForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        auto changed=ram5Request;changed[index]^=1;
        check(io->write(changed,Time(600000)).status==Completion::Failed&&kernel.writes==0,"RAM5 guard accepted mutated request");
    }
    for(auto mode:{FakeKernel::Immediate,FakeKernel::SubmitFailure,FakeKernel::Short}) {
        FakeKernel kernel;kernel.mode=mode;auto io=makeRam5DiagnosticForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        io->write(ram5Request,Time(600000));
        check(io->write(ram5Request,Time(600000)).status==Completion::Failed&&kernel.writes==1,"RAM5 guard allowed repeat");
    }
    const auto badReply=unhex("005aa5a301041400000000000000000000000000000000000000000000000000bc");
    Bytes goodReply(33);goodReply[1]=0x5a;goodReply[2]=0xa5;goodReply[3]=0xa3;goodReply[4]=1;goodReply[6]=5;goodReply[8]=222;
    for(unsigned i=3;i<32;++i)goodReply[32]+=goodReply[i];
    const auto liveGoodReply=unhex("005aa5a301000500f0009738c8cd000000000000000000000000000000000000fd");
    const auto liveA2=unhex("005aa5a20100003f2e1241124112413c0200000000000000000000000000000047");
    for(unsigned scenario=0;scenario<11;++scenario) {
        Ram5Kernel kernel;DiagnosticTrace trace;
        if(scenario==1)kernel.inputs={badReply};
        if(scenario==2)kernel.inputs={badReply,goodReply};
        if(scenario==3){kernel.pre=true;kernel.inputs=std::deque<Bytes>(64,badReply);}
        if(scenario==4)kernel.inputs=std::deque<Bytes>(64,badReply);
        if(scenario==5)kernel.failWrite=true;
        if(scenario==6)trace.failOn="ram5_single_write_intent";
        if(scenario==7)kernel.unresolved=true;
        if(scenario==8)kernel.inputs={Bytes(32)};
        if(scenario==9)kernel.inputs={liveGoodReply,liveA2};
        if(scenario==10)kernel.inputs={liveA2,liveGoodReply};
        auto io=makeRam5DiagnosticForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        const auto result=examineRam5(*io,trace,[&]{kernel.clock+=Time(1000);});
        check(kernel.writes==(scenario==3||scenario==6?0:1)&&result.writes==kernel.writes,"RAM5 diagnostic write count");
        if(kernel.writes)check(kernel.sent==ram5Request,"RAM5 diagnostic changed request");
        check(result.complete==(scenario<3||scenario==4||scenario>=9),"RAM5 diagnostic completion qualification");
        check(result.candidates==(scenario==2||scenario>=9?1:0),"unexpected report accepted as candidate");
        if(scenario>=9)check(result.after==2&&result.unexpected==1&&trace.inputs.size()==2,"live A3/A2 evidence lost or misqualified");
        if(scenario==1||scenario==2)check(trace.inputs.front()==badReply&&result.unexpected==1,"bad reply discarded");
        if(scenario==2)check(trace.inputs.size()==2&&trace.inputs.back()==goodReply,"candidate after unexpected report lost");
        if(scenario==3||scenario==4)check(trace.inputs.size()==64,"RAM5 report bound");
        if(scenario==0||scenario==1||scenario==2)check(kernel.clock==Time(850000),"RAM5 time window changed");
    }
    for(auto mode:{FakeKernel::Immediate,FakeKernel::Short,FakeKernel::SubmitFailure,FakeKernel::CompletionFailure,FakeKernel::SignaledIncomplete,FakeKernel::WaitFailure}) {
        FakeKernel kernel;kernel.mode=mode;ListenerTrace trace;
        auto io=makeInputListenerForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        const auto result=listenInput(*io,trace,[&]{kernel.clock+=Time(1000);});
        check(kernel.writes==0,"listener submitted a write");
        check(result.complete==(mode==FakeKernel::Immediate),"listener failure qualification");
        if(mode==FakeKernel::Immediate)check(result.reports==64&&kernel.reads==64&&result.stop=="report_limit","listener report ceiling");
        else check(kernel.reads==1,"listener retried failed read");
        check(io->read(Time(6000000),true).status==Completion::Failed,"listener finish did not latch stop");
    }
    for(auto mode:{FakeKernel::TimeoutCancelled,FakeKernel::Unresolved,FakeKernel::Immediate}) {
        FakeKernel kernel;kernel.mode=mode;kernel.poll=true;ListenerTrace trace;
        auto io=makeInputListenerForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        auto result=listenInput(*io,trace,[&]{kernel.clock+=Time(1000);});
        check(result.reports==0&&result.elapsed==Time(3000000)&&result.stop=="time_limit","listener time ceiling");
        check(kernel.reads==1&&kernel.writes==0&&kernel.cancels==1,"idle listener resubmitted or failed cancellation");
        check(result.complete==(mode!=FakeKernel::Unresolved),"listener unresolved cancellation hidden");
        check(io->timings().back().event==std::string("read_cancel_completion_observed"),"listener cancellation timing missing");
    }
    for(auto wire:{binding.layout.wrap(frame(1)),binding.layout.wrap(frame(0x53,Bytes{1,0x12,2,0x40,0}))}) {
        FakeKernel kernel;auto io=makeInputListenerForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        check(io->write(wire,Time(600000)).status==Completion::Failed&&kernel.submissions==0,"listener write guard bypassed");
    }
    {FakeKernel kernel;ListenerTrace trace;trace.good=false;auto io=makeInputListenerForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        check(!listenInput(*io,trace,[]{}).complete&&kernel.submissions==0,"listener read after trace failure");}
    {FakeKernel kernel;ListenerTrace trace;auto io=makeQueryIoForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        check(!listenInput(*io,trace,[]{}).complete&&kernel.submissions==0,"listener accepted read-write transport");}
    {FakeKernel kernel;auto io=makeQueryIoForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        for(unsigned i=0;i<2048;++i)if(io->read(Time(600000),true).status!=Completion::Complete)break;
        const auto submitted=kernel.submissions;
        check(submitted<2048&&!io->timingComplete(),"native timing exhaustion hidden");
        io->write(binding.layout.wrap(frame(1)),Time(600000));check(kernel.submissions==submitted,"native timing overflow allowed more submissions");}
    {FakeKernel kernel;kernel.poll=true;auto io=makeQueryIoForTest(shared,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        io->read(Time(600000),true);kernel.clock=Time(10);kernel.poll=false;
        io->write(binding.layout.wrap(frame(1)),Time(600000));kernel.clock=Time(20);io->read(Time(600000),true);
        const auto events=io->timings();check(events.size()==9,"native timing event count");
        check(events[0].event==std::string("read_submit")&&events[2].event==std::string("write_submit")&&events[8].event==std::string("read_completion_observed"),"native pending-read timing order");
        check(events[0].operation==events[8].operation&&events[0].operation!=events[2].operation&&events[2].observed==Time(10)&&events[8].observed==Time(20),"native operation/timestamp attribution");
        check(events[4].waitMetadata&&events[4].deadline==Time(600000)&&events[4].requestedWaitMs==600&&events[5].waitResult==WAIT_OBJECT_0,"native wait parameters missing");}
    for(auto mode:{AccessMode::Unknown,AccessMode::Synthetic}){bool refused=false;try{queryShareFlags(mode);}catch(const ProtocolError&){refused=true;}check(refused,"nonphysical access admitted");}
#ifdef ASB_APEX6_NEUTRAL_RUNNER
    {
        auto b=gripRehearsalFixture();b.physicalOrigin=true;b.binding.access=AccessMode::Shared;
        const std::string hash(64,'a');GripPulseApproval approval{{hash,1000,true,true,true,true,true,true,true,true,true,true,true,true,false},true,true};
        std::vector<Bytes> plan;for(const auto& phase:{gripBaselinePlan(),gripPulsePlan(b),gripBaselinePlan()})for(const auto& r:phase)plan.push_back(b.binding.layout.wrap(frame(r.command,r.payload)));
        const auto ack=unhex("005aa5530100000000000000000000000000000000000000000000000000000054");
        for(unsigned stop=0;stop<=43;++stop)for(bool cancel:{false,true}) {
            FakeKernel k;k.reply=ack;bool cancelled=false;const auto auth=GripPulseAuthorization::approve(b,approval,hash,1000);
            auto io=makeGripPulseIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),k.hooks(),[]{return 1000;},[&]{return cancelled;});
            for(unsigned i=0;i<43;++i) {
                if(i>=15&&i<=25)k.clock=Time((i-15)*8000);if(i>=26)k.clock=Time(88000);
                auto wire=plan[i];if(i==stop){if(cancel)cancelled=true;else wire[5]^=1;}
                const auto r=io->write(wire,k.clock+Time(600000));
                if(i==stop){check(r.status==Completion::Failed&&k.writes==i,"native pulse refused too late");io->write(plan[i],k.clock+Time(600000));check(k.writes==i,"native pulse latch cleared");break;}
                check(r.status==Completion::Complete&&r.submittedAt==k.clock&&r.completedAt==k.clock,"native pulse timing/result");
                if(i==14||i==26||i==27||i==28)check(io->read(k.clock+Time(600000),false).status==Completion::Complete,"native pulse ACK");
            }
            if(stop==43)check(io->write(plan[0],k.clock+Time(600000)).status==Completion::Failed&&k.writes==43,"native pulse extra write");
            bool refused=false;HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            try{auto reused=makeGripPulseIoForTest(auth,event,k.hooks(),[]{return 1000;},[]{return false;});}catch(const ProtocolError&){refused=true;}
            check(refused,"native pulse authorization reuse"); // factory owns event even on refusal
        }
        for(unsigned scenario=0;scenario<7;++scenario) {
            FakeKernel k;k.reply=ack;auto hooks=k.hooks();std::int64_t wall=1000;bool cancelled=false;
            const auto auth=GripPulseAuthorization::approve(b,approval,hash,1000);
            // Simulate work between the outer write check and the final native check.
            unsigned clockCalls=0;bool stall=false;
            hooks.clock=[&]{if(stall&&++clockCalls==4)k.clock+=Time(10001);return k.clock;};
            auto io=makeGripPulseIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),hooks,[&]{return wall;},[&]{return cancelled;});
            for(unsigned i=0;i<14;++i)check(io->write(plan[i],Time(600000)).status==Completion::Complete,"pulse preflight setup");
            if(scenario==0){wall=1301;check(io->write(plan[14],Time(600000)).status==Completion::Failed&&k.writes==14,"pulse entry expiry");continue;}
            check(io->write(plan[14],Time(600000)).status==Completion::Complete,"pulse entry setup");
            if(scenario==1){k.reply=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");}
            io->read(Time(600000),false);
            if(scenario==2)stall=true;
            if(scenario==3)k.mode=FakeKernel::Short;
            if(scenario==4){auto unused=io->read(Time(600000),true);}
            if(scenario==5)cancelled=true;
            if(scenario==6)k.clock=Time(10001);
            const auto r=io->write(plan[15],Time(600000));check(r.status!=Completion::Complete,"native pulse fault accepted");
            check(k.writes==(scenario==3?16:15),"native pulse faulty report submitted");
            io->write(plan[15],Time(600000));check(k.writes==(scenario==3?16:15),"native pulse fault retried");
        }
        HANDLE cancel=CreateEventW(nullptr,TRUE,TRUE,nullptr);
        {WindowsPulseWait wait(cancel,[]{return Time{};});check(wait.cancelled(),"pulse timer ignored cancel");wait.waitUntil(Time(8000));}
        CloseHandle(cancel);
        // Final spin remains cancellable and bounded on invalid clocks.
        HANDLE spinCancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        unsigned ticks=0;
        {WindowsPulseWait wait(spinCancel,[&]{return Time(100*ticks++);});wait.waitUntil(Time(900));check(ticks>=10,"pulse spin returned early");}
        ticks=0;
        {WindowsPulseWait wait(spinCancel,[&]{if(++ticks==4)SetEvent(spinCancel);return Time(50*ticks);});wait.waitUntil(Time(900));check(wait.cancelled()&&ticks<18,"pulse spin ignored cancellation");}
        ResetEvent(spinCancel);
        for(unsigned fault=0;fault<3;++fault) {
            ticks=0;bool refused=false;
            try{WindowsPulseWait wait(spinCancel,[&]{return fault==0?Time(ticks++?100:200):Time{};});wait.waitUntil(Time(fault==2?10001:900));}catch(const ProtocolError&){refused=true;}
            check(refused,"pulse spin accepted reversed/stalled clock or excessive wait");
        }
        CloseHandle(spinCancel);
    }
    {
        auto baseline=rehearsalFixture();baseline.binding.access=AccessMode::Shared;
        const std::string hash(64,'a');Approval approval{hash,1000,true,true,true,true,true,true,true,true,true,true};
        const auto authorization=NeutralAuthorization::approve(baseline,approval,hash,1000);
        std::vector<Bytes> reports;
        for(const auto& phase:{snapshotPlan(baseline),neutralPlan(baseline),snapshotPlan(baseline)})for(const auto& r:phase)reports.push_back(baseline.binding.layout.wrap(frame(r.command,r.payload)));
        for(unsigned corrupt=0;corrupt<=reports.size();++corrupt) {
            FakeKernel kernel;auto io=makeNeutralIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(unsigned n=0;n<reports.size();++n) {
                auto wire=reports[n];if(n==corrupt)wire.back()^=1;
                auto result=io->write(wire,Time(600000));
                if(n==corrupt){check(result.status==Completion::Failed&&kernel.submissions==n,"native altered report submitted");io->write(reports[n],Time(600000));check(kernel.submissions==n,"native retry after fault");break;}
                check(result.status==Completion::Complete,"native exact report refused");
            }
            if(corrupt==reports.size()){check(kernel.submissions==reports.size(),"native plan incomplete");check(io->write(reports[0],Time(600000)).status==Completion::Failed&&kernel.submissions==reports.size(),"extra native request allowed");}
        }
        for(auto forbidden:{gripWaveform(std::array<std::array<double,2>,1>{{{0.1,0.0}}}),frame(0xa4),frame(0x53,Bytes{1,0x10,2})}) {
            FakeKernel kernel;auto io=makeNeutralIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            check(io->write(baseline.binding.layout.wrap(forbidden),Time(600000)).status==Completion::Failed&&kernel.submissions==0,"arbitrary native command allowed");
        }
        for(unsigned target=0;target+1<reports.size();++target) {
            if(reports[target]==reports[target+1])continue;
            FakeKernel kernel;auto io=makeNeutralIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(unsigned n=0;n<target;++n)io->write(reports[n],Time(600000));
            check(io->write(reports[target+1],Time(600000)).status==Completion::Failed&&kernel.submissions==target,"native accepted reordered valid report");
        }
        {FakeKernel kernel;std::int64_t now=1000;auto io=makeNeutralIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[&]{return now;});
            for(const auto& wire:reports){if(wire[3]==0x53){now=1301;const auto count=kernel.submissions;check(io->write(wire,Time(600000)).status==Completion::Failed&&kernel.submissions==count,"native expired entry allowed");break;}io->write(wire,Time(600000));}}
    }
    {
        auto baseline=gripRehearsalFixture();baseline.physicalOrigin=true;baseline.binding.access=AccessMode::Shared;
        const std::string hash(64,'a');GripApproval approval{{hash,1000,true,true,true,true,true,true,true,true,true,true},true};
        const auto authorization=GripNeutralAuthorization::approve(baseline,approval,hash,1000);
        std::vector<Bytes> reports;
        for(const auto& plan:{gripBaselinePlan(),neutralPlan(baseline),gripBaselinePlan()})for(const auto& r:plan)reports.push_back(baseline.binding.layout.wrap(frame(r.command,r.payload)));
        check(reports.size()==49,"native grip lifecycle size");
        {FakeKernel kernel;auto io=makeGripNeutralIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(const auto& wire:reports)check(io->write(wire,Time(600000)).status==Completion::Complete,"native grip lifecycle refused");
            check(io->write(reports[0],Time(600000)).status==Completion::Failed&&kernel.writes==49,"native grip exceeded budget");}
        for(unsigned target=0;target<reports.size();++target) {
            FakeKernel kernel;auto io=makeGripNeutralIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(unsigned i=0;i<target;++i)io->write(reports[i],Time(600000));
            auto invalid=reports[target];invalid[5]^=1;
            check(io->write(invalid,Time(600000)).status==Completion::Failed&&kernel.writes==target,"native grip accepted changed report");
            check(io->write(reports[target],Time(600000)).status==Completion::Failed&&kernel.writes==target,"native grip retry after refusal");
        }
        {FakeKernel kernel;std::int64_t now=1000;auto io=makeGripNeutralIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[&]{return now;});
            for(unsigned i=0;i<24;++i)io->write(reports[i],Time(600000));now=1301;
            check(io->write(reports[24],Time(600000)).status==Completion::Failed&&kernel.writes==24,"native grip expired entry accepted");}
        for(auto forbidden:{gripWaveform(std::array<std::array<double,2>,1>{{{0.1,0.0}}}),frame(0xa4),frame(0xa3,Bytes{1,5})}) {
            FakeKernel kernel;auto io=makeGripNeutralIoForTest(authorization,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(unsigned i=0;i<27;++i)io->write(reports[i],Time(600000));
            check(io->write(baseline.binding.layout.wrap(forbidden),Time(600000)).status==Completion::Failed&&kernel.writes==27,"native grip allowed nonzero waveform or unrelated RAM/write");
        }
    }
    {
        auto baseline=gripRehearsalFixture();baseline.physicalOrigin=true;baseline.binding.access=AccessMode::Shared;
        const std::string hash(64,'d');const GripRestoreApproval approval{hash,1000,true,true,true,true,true,true,true,true,true,true,true,true};
        const auto auth=GripRestoreAuthorization::approve(baseline,approval,hash,1000);
        std::vector<Bytes> reports;for(const auto& plan:{gripBaselinePlan(),gripRestoreLeftPlan(baseline),gripBaselinePlan()})
            for(const auto& r:plan)reports.push_back(baseline.binding.layout.wrap(frame(r.command,r.payload)));
        check(reports.size()==29,"native restore-only count");
        {FakeKernel kernel;auto io=makeGripRestoreIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(const auto& w:reports)check(io->write(w,Time(600000)).status==Completion::Complete,"native restore-only refused ordered plan");
            check(io->write(reports[14],Time(600000)).status==Completion::Failed&&kernel.writes==29,"native restore-only permitted repeat");}
        for(unsigned target=0;target<reports.size();++target) {
            FakeKernel kernel;auto io=makeGripRestoreIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(unsigned i=0;i<target;++i)io->write(reports[i],Time(600000));auto invalid=reports[target];invalid[5]^=1;
            check(io->write(invalid,Time(600000)).status==Completion::Failed&&kernel.writes==target,"native restore-only changed report accepted");
            check(io->write(reports[target],Time(600000)).status==Completion::Failed&&kernel.writes==target,"native restore-only fault not latched");
        }
        for(const auto& forbidden:{frame(0x53,Bytes{1,0x12,2,0x40,0}),frame(0x53,Bytes{1,0x12,0}),frame(0x53,Bytes{1,0x11,1,0x40}),gripWaveform({}),frame(0xa3,Bytes{1,5})}) {
            FakeKernel kernel;auto io=makeGripRestoreIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(unsigned i=0;i<14;++i)io->write(reports[i],Time(600000));
            check(io->write(baseline.binding.layout.wrap(forbidden),Time(600000)).status==Completion::Failed&&kernel.writes==14,"native restore-only broadened effects");
        }
        {FakeKernel kernel;std::int64_t now=1000;auto io=makeGripRestoreIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[&]{return now;});
            for(unsigned i=0;i<14;++i)io->write(reports[i],Time(600000));now=1301;
            check(io->write(reports[14],Time(600000)).status==Completion::Failed&&kernel.writes==14,"native restore-only expired approval accepted");}
    }
    {
        auto baseline=gripRehearsalFixture();baseline.physicalOrigin=true;baseline.binding.access=AccessMode::Shared;
        const std::string hash(64,'e');GripRestoreApproval approval{hash,1000,true,true,true,true,true,true,true,true,true,true,true,true};
        approval.observeRepliesAccepted=true;const auto auth=GripRestoreAuthorization::approve(baseline,approval,hash,1000,true);
        std::vector<Bytes> reports;for(const auto& plan:{gripBaselinePlan(),gripRestoreLeftPlan(baseline)})
            for(const auto& r:plan)reports.push_back(baseline.binding.layout.wrap(frame(r.command,r.payload)));
        check(reports.size()==15,"native observation plan broadened");
        for(unsigned target=0;target<15;++target) {
            FakeKernel kernel;auto io=makeGripRestoreIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(unsigned i=0;i<target;++i)io->write(reports[i],Time(600000));auto invalid=reports[target];invalid[5]^=1;
            check(io->write(invalid,Time(600000)).status==Completion::Failed&&kernel.writes==target,"native observation mutation accepted");
        }
        for(const auto& extra:{reports[0],reports[14],baseline.binding.layout.wrap(gripWaveform({})),baseline.binding.layout.wrap(frame(0x53,Bytes{1,0x11,1,0x40}))}) {
            FakeKernel kernel;auto io=makeGripRestoreIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(const auto& w:reports)check(io->write(w,Time(600000)).status==Completion::Complete,"native observation ordered write failed");
            check(io->read(Time(500000),false).status==Completion::Complete,"observation read after final allowed write failed");
            check(io->write(extra,Time(600000)).status==Completion::Failed&&kernel.writes==15,"native observation allowed postflight/extra effect");
        }
        for(auto mode:{FakeKernel::TimeoutCancelled,FakeKernel::Unresolved}) {
            FakeKernel kernel;auto io=makeGripRestoreIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            for(const auto& w:reports)io->write(w,Time(600000));kernel.mode=mode;
            check(io->read(Time(500000),false).status==(mode==FakeKernel::Unresolved?Completion::Unresolved:Completion::Timeout),"observation cancellation status");
            check(io->finish()==(mode!=FakeKernel::Unresolved)&&kernel.writes==15,"observation finalization hid unresolved I/O");
        }
    }
#endif
    {
        const std::vector<std::pair<DWORD,DWORD>> expected{{0,3},{GENERIC_READ,0},{GENERIC_READ,1},{GENERIC_READ,3},{GENERIC_READ|GENERIC_WRITE,0},{GENERIC_READ|GENERIC_WRITE,1},{GENERIC_READ|GENERIC_WRITE,3}};
        for(const auto failure:{ERROR_SUCCESS,ERROR_SHARING_VIOLATION,ERROR_ACCESS_DENIED,ERROR_DEVICE_NOT_CONNECTED}) {
            unsigned opens=0,closes=0;bool live=false;
            auto results=probeInterfaceAccessForTest(binding,{
                [&](const std::wstring& path,DWORD access,DWORD share,DWORD& error)->HANDLE {
                    check(!live,"probe overlapped its own handles");check(path==wide(binding.path),"probe changed device path");
                    check(opens<expected.size()&&expected[opens]==std::pair<DWORD,DWORD>{access,share},"probe access matrix changed");++opens;
                    error=failure;if(error)return INVALID_HANDLE_VALUE;live=true;return reinterpret_cast<HANDLE>(1);
                },
                [&](HANDLE handle,DWORD& error)->BOOL {check(live&&handle==reinterpret_cast<HANDLE>(1),"probe closed wrong handle");live=false;++closes;error=123;return TRUE;}
            });
            check(opens==(failure==ERROR_DEVICE_NOT_CONNECTED?1:7),"probe retried unexpected error or skipped a case");
            check(results.size()==opens&&!live,"probe result count or ownership mismatch");
            check(closes==(failure==ERROR_SUCCESS?opens:0),"probe closed failed open or leaked success");
            for(const auto& r:results)check(r.opened==(failure==ERROR_SUCCESS)&&r.closed==r.opened&&r.openError==failure&&r.closeError==0,"probe error/result normalization");
        }
        unsigned opens=0,closes=0;
        WindowsAccessHooks failedClose{
            [&](const std::wstring&,DWORD,DWORD,DWORD& error)->HANDLE{++opens;error=55;return reinterpret_cast<HANDLE>(1);},
            [&](HANDLE,DWORD& error)->BOOL{++closes;error=ERROR_INVALID_HANDLE;return FALSE;}
        };
        auto results=probeInterfaceAccessForTest(binding,failedClose);
        check(opens==1&&closes==1&&results.size()==1,"probe continued after close failure");
        check(results[0].opened&&!results[0].closed&&results[0].openError==0&&results[0].closeError==ERROR_INVALID_HANDLE,"probe hid close failure");
        auto invalid=binding;invalid.path.clear();bool refused=false;
        try{probeInterfaceAccessForTest(invalid,failedClose);}catch(const ProtocolError&){refused=true;}
        check(refused&&opens==1,"probe opened missing identity");
    }
#ifdef ASB_APEX6_NEUTRAL_RUNNER
    {
        auto baseline=gripRehearsalFixture();baseline.physicalOrigin=true;baseline.binding.access=AccessMode::Shared;
        const std::string hash(64,'a');GripLifecycleApproval approval{{hash,1000,true,true,true,true,true,true,true,true,true,true,true,true}};
        std::vector<Bytes> sequence;
        for(const auto& phase:{gripBaselinePlan(),gripLifecyclePlan(baseline),gripBaselinePlan()})for(const auto& r:phase)sequence.push_back(baseline.binding.layout.wrap(frame(r.command,r.payload)));
        for(unsigned scenario=0;scenario<10;++scenario) {
            auto auth=GripLifecycleAuthorization::approve(baseline,approval,hash,1000);
            FakeKernel kernel;auto io=makeGripLifecycleIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});
            const unsigned stop=scenario==0?32:scenario==8?15:14;
            for(unsigned i=0;i<stop;++i)check(io->write(sequence[i],kernel.clock+Time(600000)).status==Completion::Complete,"native lifecycle ordered write rejected");
            auto invalid=sequence[14];
            if(scenario==1)invalid=sequence[15]; // reordered exit
            if(scenario==2)invalid=baseline.binding.layout.wrap(frame(0x53,Bytes{1,0x12,2,0x41,0}));
            if(scenario==3)invalid=baseline.binding.layout.wrap(gripWaveform({}));
            if(scenario==4)invalid=baseline.binding.layout.wrap(frame(0xa4));
            if(scenario==5)invalid=baseline.binding.layout.wrap(frame(0x53,Bytes{1,0,0}));
            if(scenario==6)invalid=sequence[0];
            if(scenario==7)invalid=sequence[17];
            if(scenario==8){kernel.clock=Time(5000000);invalid=sequence[15];}
            if(scenario==9)kernel.clock=Time(75000000);
            const auto count=kernel.submissions;
            check(io->write(invalid,kernel.clock+Time(600000)).status==Completion::Failed&&kernel.submissions==count,"native lifecycle guard bypass");
            check(io->write(sequence[0],kernel.clock+Time(600000)).status==Completion::Failed&&kernel.submissions==count,"native lifecycle failure unlatched");
            bool refused=false;try{makeGripLifecycleIoForTest(auth,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks(),[]{return 1000;});}catch(const ProtocolError&){refused=true;}
            check(refused,"native lifecycle approval reused");
        }
        for(unsigned changed:{23u,24u,25u,26u}) {
            auto s=baseline;s.mapping[changed]^=1;
            // Recompute the fingerprint to test semantic allowlisting, not CRC failure.
            std::uint32_t crc=0xffffffffu;for(auto b:s.mapping){crc^=b;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0u);}s.motor.crc32=~crc;
            bool refused=false;try{GripLifecycleAuthorization::approve(s,approval,hash,1000);}catch(const ProtocolError&){refused=true;}
            check(refused,"native lifecycle changed saved mode/parameter admitted");
        }
    }
#endif
    for(auto mode:{FakeKernel::Immediate,FakeKernel::Pending,FakeKernel::Short,FakeKernel::TimeoutCancelled,FakeKernel::TimeoutLateSuccess,FakeKernel::Unresolved,FakeKernel::SubmitFailure,FakeKernel::CompletionFailure,FakeKernel::SignaledIncomplete,FakeKernel::WaitFailure}) {
        FakeKernel kernel;kernel.mode=mode;HANDLE inert=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        auto io=makeQueryIoForTest(binding,inert,kernel.hooks());auto result=io->write(binding.layout.wrap(frame(1)),Time(600000));
        check(kernel.submissions==1,"write submitted more than once");
        if(mode==FakeKernel::Immediate||mode==FakeKernel::Pending)check(result.status==Completion::Complete&&result.transferred==33,"completion lost");
        if(mode==FakeKernel::Short)check(result.transferred==32,"short count hidden");
        if(mode==FakeKernel::TimeoutLateSuccess)check(result.status==Completion::Timeout&&result.transferred==33,"late cancellation success incorrectly accepted");
        if(mode==FakeKernel::TimeoutCancelled)check(result.status==Completion::Timeout&&kernel.cancels==1,"cancellation policy");
        if(mode==FakeKernel::WaitFailure)check(result.status==Completion::Failed&&result.error==ERROR_INVALID_HANDLE,"wait failure disguised as timeout or cancellation");
        if(mode==FakeKernel::Unresolved||mode==FakeKernel::SignaledIncomplete) {
            check(result.status==Completion::Unresolved,"unresolved completion hidden");
            auto operation=kernel.operations.begin()->first;auto buffer=static_cast<std::uint8_t*>(kernel.operations.begin()->second.buffer);auto event=operation->hEvent;
            io.reset();DWORD flags=0;
            check(GetHandleInformation(event,&flags)!=FALSE&&GetHandleInformation(inert,&flags)!=FALSE,"unresolved handles freed");
            check(buffer[1]==0x5a&&operation->hEvent==event,"unresolved buffers freed");
            // Retained objects intentionally live until this test process exits,
            // exactly as they would in a terminated supervised hardware worker.
        }else if(mode!=FakeKernel::Immediate&&mode!=FakeKernel::Pending&&mode!=FakeKernel::Short) {
            io->write(binding.layout.wrap(frame(1)),Time(600000));check(kernel.submissions==1,"native adapter resubmitted after fault");
        }
    }
    // Model the observed sub-millisecond early wake, plus error/cancel races.
    // Every case uses one inert handle and at most ONE submitted operation.
    for(bool reading:{false,true})for(unsigned scenario=0;scenario<10;++scenario) {
        FakeKernel kernel;kernel.mode=FakeKernel::TimeoutCancelled;unsigned waits=0;
        auto hooks=kernel.hooks();
        hooks.wait=[&](HANDLE,DWORD milliseconds)->DWORD {
            ++waits;
            if(scenario==2)return WAIT_TIMEOUT; // stalled clock: bounded by wake count
            if(scenario==3||scenario==4||scenario==9) {
                if(scenario==4)kernel.mode=FakeKernel::Unresolved;
                SetLastError(ERROR_ACCESS_DENIED);return scenario==9?WAIT_ABANDONED:WAIT_FAILED;
            }
            if(scenario==6){kernel.clock=Time(-1);return WAIT_TIMEOUT;}
            if(waits==1){check(milliseconds==500,"wrong first wait duration");kernel.clock=Time(499605);return WAIT_TIMEOUT;}
            check(milliseconds==1,"early wake did not round remaining 395 us to 1 ms");
            if(scenario==1){kernel.clock=Time(499800);kernel.mode=FakeKernel::Immediate;return WAIT_OBJECT_0;}
            if(scenario==5){SetLastError(ERROR_INVALID_HANDLE);return WAIT_FAILED;}
            kernel.clock=Time(500000);
            if(scenario==7)kernel.mode=FakeKernel::TimeoutLateSuccess;
            if(scenario==8)kernel.mode=FakeKernel::CompletionFailure;
            return WAIT_TIMEOUT;
        };
        auto io=makeQueryIoForTest(binding,CreateEventW(nullptr,TRUE,FALSE,nullptr),hooks);
        const auto result=reading?io->read(Time(500000),false):io->write(binding.layout.wrap(frame(1)),Time(500000));
        check(kernel.submissions==1&&waits<=8,"early wait wake resubmitted I/O or exceeded wake limit");
        if(scenario==0)check(result.status==Completion::Timeout&&result.error==ERROR_TIMEOUT&&kernel.cancels==1&&waits==2,"early timeout not held to original deadline");
        if(scenario==1)check(result.status==Completion::Complete&&kernel.cancels==0&&waits==2,"completion after early wake lost");
        if(scenario==2)check(result.status==Completion::Failed&&result.error==ERROR_RETRY&&waits==8,"stalled clock loop unbounded");
        if(scenario==3||scenario==5)check(result.status==Completion::Failed&&result.error==(scenario==3?ERROR_ACCESS_DENIED:ERROR_INVALID_HANDLE),"original wait error overwritten by aborted cancellation");
        if(scenario==4)check(result.status==Completion::Unresolved&&result.error==ERROR_ACCESS_DENIED&&!io->finish(),"unresolved wait-failure cancellation hidden");
        if(scenario==6||scenario==9)check(result.status==Completion::Failed&&result.error==ERROR_INVALID_DATA,"clock reversal/unexpected wait result accepted");
        if(scenario==7)check(result.status==Completion::Timeout&&result.transferred==33&&(!reading||result.bytes.size()==33),"late cancel race promoted to success or lost raw bytes");
        if(scenario==8)check(result.status==Completion::Failed&&result.error==ERROR_DEVICE_NOT_CONNECTED,"completion failure disguised as silent timeout");
        unsigned returns=0;bool sawCancel=false;
        for(const auto& event:io->timings()) {
            if(std::string(event.event).ends_with("wait_return")) {
                ++returns;check(event.waitMetadata&&event.deadline==Time(500000),"wait deadline metadata changed");
                if(scenario==3||scenario==4)check(event.waitResult==WAIT_FAILED&&event.error==ERROR_ACCESS_DENIED,"wait return code/error evidence missing");
            }
            if(std::string(event.event).ends_with("cancel_requested"))sawCancel=true;
        }
        check(returns==waits&&sawCancel==(scenario!=1),"wait/cancellation chronology incomplete");
        if(scenario!=1){io->write(binding.layout.wrap(frame(1)),Time(600000));check(kernel.submissions==1,"write resumed after wait failure");}
    }
    for(auto f:{frame(0x53,Bytes{1,0x12,2,0x40,0}),gripWaveform({}),frame(0xa4),frame(0xa3,Bytes{2,6}),frame(0xa3,Bytes{0,6,0,15})}) {
        FakeKernel kernel;auto io=makeQueryIoForTest(binding,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        check(io->write(binding.layout.wrap(f),Time(600000)).status==Completion::Failed&&kernel.submissions==0,"physical allowlist admitted write");
    }
    {FakeKernel kernel;kernel.poll=true;auto io=makeQueryIoForTest(binding,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        check(io->read(Time(600000),true).status==Completion::Idle,"empty poll");
        check(kernel.submissions==1&&kernel.cancels==0,"empty poll must retain read");
        kernel.poll=false;auto result=io->read(Time(600000),false);check(result.status==Completion::Complete&&kernel.submissions==1,"pending read was restarted");}
    {FakeKernel kernel;kernel.poll=true;kernel.mode=FakeKernel::Unresolved;auto io=makeQueryIoForTest(binding,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        io->read(Time(600000),true);auto operation=kernel.operations.begin()->first;io.reset();DWORD flags=0;check(kernel.cancels==1&&GetHandleInformation(operation->hEvent,&flags),"destructor freed pending read");}
    {FakeKernel kernel;kernel.clock=Time(600000);auto io=makeQueryIoForTest(binding,CreateEventW(nullptr,TRUE,FALSE,nullptr),kernel.hooks());
        check(io->write(binding.layout.wrap(frame(1)),Time(600000)).status==Completion::Timeout&&kernel.submissions==0,"expired operation submitted");}
    std::cout<<checks<<" Windows I/O lifetime checks passed using inert handles; no HID opens\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
