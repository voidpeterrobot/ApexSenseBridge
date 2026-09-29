#include "apex6/experiment/Session.h"
#include "apex6/experiment/FakeIo.h"
#include "apex6/experiment/Evidence.h"
#include "apex6/experiment/ApprovalConsole.h"
#include "apex6_rehearsal_fixture.h"
#include "fixtures/apex6/OfficialSilent.h"
#include <atomic>
#include <iostream>
#include <future>
#include <sstream>

using namespace asb::apex6;
using namespace asb::apex6::experiment;
namespace {
unsigned checks=0;
void check(bool b,const char* msg){++checks;if(!b)throw std::runtime_error(msg);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const ProtocolError&){caught=true;}check(caught,"expected refusal");}
struct MemoryTrace:Trace {
    bool ok=true;unsigned count=0,failAt=0;std::vector<std::string> events;
    void record(Time,const std::string& event,std::span<const std::uint8_t>,std::uint64_t,std::uint64_t)override {
        ++count;events.push_back(event);if(count==failAt){ok=false;throw ProtocolError("injected trace fault");}
    }
    bool healthy()const override{return ok;}
};
Request info(){return {1,{},25};}
void unchangedAfterFault(Session& session,FakeIo& io) {
    const auto writes=io.writes,reads=io.reads;
    rejects([&]{session.exchange(info());});rejects([&]{session.begin("retry",Time(1),1);});
    check(io.writes==writes&&io.reads==reads,"post-fault device traffic");
}
void codecAndSnapshots() {
    auto fixture=rehearsalFixture();auto encoded=encodeSnapshot(fixture);
    check(decodeSnapshot(encoded)==fixture,"snapshot round trip");
    for(auto mode:{AccessMode::Exclusive,AccessMode::Shared,AccessMode::Synthetic}) {auto s=fixture;s.binding.access=mode;check(decodeSnapshot(encodeSnapshot(s))==s,"v2 access mode lost");}
    auto legacy=fixture;legacy.schema=1;legacy.binding.access=AccessMode::Unknown;
    check(decodeSnapshot(encodeSnapshot(legacy))==legacy,"legacy snapshot round trip");
    {FakeIo io(legacy);MemoryTrace trace;Session session(io,trace);check(rehearseNeutral(session,legacy).complete,"legacy offline rehearsal broken");}
    rejects([&]{decodeSnapshot(encoded+"\n");});rejects([&]{decodeSnapshot(encoded.substr(0,encoded.size()-10));});
    rejects([&]{decodeSnapshot(std::string(65537,'0'));});
    auto bad=fixture;bad.blocks[0][0]^=1;rejects([&]{validateSnapshot(bad);});
    for(auto size:{33,64,65})for(auto id:{0,3,7}) {
        Layout l{static_cast<std::uint8_t>(id),static_cast<std::uint8_t>(id),static_cast<std::uint16_t>(size),static_cast<std::uint16_t>(size)};
        auto bytes=l.wrap(frame(1));check(bytes.size()==size&&bytes[0]==id,"ID envelope");check(l.unwrap(bytes).size()==size-1,"Windows input ID stripped");
        bytes[0]^=1;rejects([&]{l.unwrap(bytes);});bytes.pop_back();rejects([&]{l.unwrap(bytes);});
    }
    rejects([]{Layout{0,0,32,33}.validate();});rejects([]{Layout{0,0,33,66}.validate();});
    rejects([]{identity(Bytes(24));});auto p=Bytes(25);p[0]=149;p[24]=128;rejects([&]{identity(p);});p[0]=150;p[24]=0;rejects([&]{identity(p);});
    rejects([]{uid(Bytes(16));});rejects([]{uid(Bytes(16,255));});rejects([]{versions(Bytes(5));});
    for(auto width:{33,65}) {
        auto s=fixture;s.binding.layout.inputLength=static_cast<std::uint16_t>(width);s.binding.layout.outputLength=static_cast<std::uint16_t>(width);
        FakeIo io(s);MemoryTrace trace;Session session(io,trace);session.begin("snapshot",Time(30000000),128);
        check(acquireSnapshot(session)==s,"complete fake snapshot");session.end();check(io.writes==snapshotPlan(s).size(),"snapshot exact request count");
    }
}
void successAndEveryFailure() {
    auto baseline=rehearsalFixture();FakeIo successful(baseline);MemoryTrace good;Session s(successful,good);
    auto result=rehearseNeutral(s,baseline);check(result.complete,"neutral success rehearsal");
    check(neutralPlan(baseline).size()==21,"reference 21-request active plan");
    const auto total=successful.writes;
    for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved})for(unsigned n=1;n<=total;++n) {
        FakeIo io(baseline);io.failAt=n;io.failStatus=status;MemoryTrace trace;Session session(io,trace);
        auto failure=rehearseNeutral(session,baseline);
        check(!failure.complete&&session.failed()&&io.writes==n,"failure must latch at exact request");unchangedAfterFault(session,io);
        if(n>=snapshotPlan(baseline).size()+11)check(failure.deviceStateUncertain,"entry attempt must mark uncertain");
    }
    // Every trace failure, including write-intent and parsing, must stop traffic.
    for(unsigned n=1;n<=good.count;++n) {
        FakeIo io(baseline);MemoryTrace trace;trace.failAt=n;Session session(io,trace);
        auto failure=rehearseNeutral(session,baseline);check(!failure.complete,"trace failure accepted");unchangedAfterFault(session,io);
    }
    FakeIo physical(baseline);physical.pretendPhysical=true;MemoryTrace trace;Session locked(physical,trace);
    check(!rehearseNeutral(locked,baseline).complete&&physical.writes==0&&physical.reads==0,"physical neutral must refuse before traffic");
}
void faults() {
    const auto fixture=rehearsalFixture();
    for(unsigned scenario=0;scenario<12;++scenario) {
        FakeIo io(fixture);MemoryTrace trace;Session session(io,trace);session.begin("snapshot",Time(30000000),128);
        if(scenario==0)io.shortWrite=true;
        if(scenario==1)io.present=false;
        if(scenario==2)io.flood=true;
        if(scenario==3)io.readDelay=Time(600000);
        if(scenario==4)io.clock=Time(30000000);
        if(scenario==5)io.snapshot.binding.container="replacement";
        if(scenario==6)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==4)p[0]^=1;}; // stable changed UID compared below
        if(scenario==7)io.responseHook=[](auto& fake,auto cmd,auto& p){if(cmd==0xa1&&fake.writes>9)p[0]=1;};
        if(scenario==8)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==0xa3)p[1]=1;};
        if(scenario==9)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==1)p[0]=149;};
        if(scenario==10)io.writeDelay=Time(600001);
        if(scenario==11)trace.ok=false;
        if(scenario==6) {
            auto changed=acquireSnapshot(session);check(changed.unit!=fixture.unit,"fixture changes detectable");
            // Snapshot discovery may identify a new UID; approved baseline comparison must refuse it.
            FakeIo other(fixture);other.responseHook=io.responseHook;MemoryTrace tr;Session bounded(other,tr);
            check(!rehearseNeutral(bounded,fixture).complete,"changed UID baseline accepted");unchangedAfterFault(bounded,other);
        }else {rejects([&]{acquireSnapshot(session);});unchangedAfterFault(session,io);}
        if(scenario==2)check(io.reads==64&&io.writes==0,"drain must cap at 64 reports");
    }
    // An old matching response in the drain is evidence of uncertain attribution.
    FakeIo stale(fixture);MemoryTrace trace;Session session(stale,trace);session.begin("snapshot",Time(30000000),128);
    Bytes old(33);old[1]=0x5a;old[2]=0xa5;old[3]=1;stale.incoming.push_back(old);
    rejects([&]{session.exchange(info());});check(stale.writes==0,"stale reply allowed new command");
    FakeIo ordered(fixture);MemoryTrace tr;Session exact(ordered,tr);exact.begin("plan",Time(30000000),128,snapshotPlan(fixture));
    rejects([&]{exact.exchange({4,{},16});});check(ordered.writes==0,"out-of-order request submitted");
    FakeIo budget(fixture);MemoryTrace t;Session limited(budget,t);limited.begin("budget",Time(30000000),1);limited.exchange(info());rejects([&]{limited.exchange(info());});check(budget.writes==1,"attempt limit exceeded");
    for(unsigned target=0;target<4;++target) {
        FakeIo io(fixture);unsigned modes=0;
        io.responseHook=[&](auto&,auto cmd,auto& p){if(cmd==0x53&&modes++==target)p[0]=1;};
        MemoryTrace tr2;Session m(io,tr2);auto r=rehearseNeutral(m,fixture);check(!r.complete&&r.deviceStateUncertain,"mode refusal ignored");unchangedAfterFault(m,io);
    }
}
void approvals() {
    const std::string hash(64,'a');Approval a{hash,1000,true,true,true,true,true,true,true,true,true,true};
    check(validApproval(a,hash,1000)&&validApproval(a,hash,1300),"fresh approval refused");
    check(!validApproval(a,hash,999)&&!validApproval(a,hash,1301)&&!validApproval(a,std::string(64,'b'),1000),"stale/mismatched approval accepted");
    for(auto member:{&Approval::approved,&Approval::observer,&Approval::powerOffReady,&Approval::normalVibration,&Approval::knownWritersQuiesced,&Approval::directUsb,&Approval::allDisabledAccepted,&Approval::failStopAccepted,&Approval::backgroundRiskAccepted,&Approval::restorationRiskAccepted}) {auto b=a;b.*member=false;check(!validApproval(b,hash,1000),"missing operator checkpoint accepted");}
    check(encodeApproval(decodeApproval(encodeApproval(a)))==encodeApproval(a),"approval codec");
    rejects([&]{decodeApproval(encodeApproval(a)+"\n");});rejects([&]{decodeApproval("ASB_APEX6_APPROVAL_V1\n");});
    auto baseline=rehearsalFixture();rejects([&]{NeutralAuthorization::approve(baseline,a,hash,1000);});
    baseline.schema=1;baseline.binding.access=AccessMode::Unknown;rejects([&]{NeutralAuthorization::approve(baseline,a,hash,1000);});
    baseline.schema=2;baseline.binding.access=AccessMode::Shared;
    auto auth=NeutralAuthorization::approve(baseline,a,hash,1000);
    rejects([&]{NeutralAuthorization::approve(baseline,a,std::string(64,'b'),1000);});rejects([&]{auth.check(1301);});
    FakeIo good(baseline);good.pretendPhysical=true;MemoryTrace trace;Session session(good,trace);
    auto result=runAuthorizedNeutral(session,auth,[]{return 1000;});
    check(result.complete&&session.queryAttempts()==116&&session.actuatorAttempts()==7&&result.deviceStateUncertain,"authorized lifecycle/counts");
    for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved})for(unsigned n=1;n<=good.writes;++n) {
        FakeIo io(baseline);io.pretendPhysical=true;io.failAt=n;io.failStatus=status;MemoryTrace tr;Session s(io,tr);
        auto r=runAuthorizedNeutral(s,auth,[]{return 1000;});check(!r.complete&&io.writes==n,"authorized fault not latched");unchangedAfterFault(s,io);
    }
    for(unsigned n=1;n<=trace.count;++n) {
        FakeIo io(baseline);io.pretendPhysical=true;MemoryTrace tr;tr.failAt=n;Session s(io,tr);
        check(!runAuthorizedNeutral(s,auth,[]{return 1000;}).complete,"authorized trace failure accepted");unchangedAfterFault(s,io);
    }
    for(unsigned scenario=0;scenario<5;++scenario) {
        FakeIo io(baseline);io.pretendPhysical=true;MemoryTrace tr;Session s(io,tr);
        unsigned clocks=0;
        if(scenario==1)io.snapshot.binding.access=AccessMode::Exclusive;
        if(scenario==2)io.present=false;
        if(scenario==3)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==4)p[0]^=1;};
        const auto r=runAuthorizedNeutral(s,auth,[&]{++clocks;return scenario==4|| (scenario==0&&clocks>1)?1301:1000;});
        check(!r.complete&&s.actuatorAttempts()==0,"entry proceeded after expiry/binding/removal/UID failure");
        unchangedAfterFault(s,io);
    }
}
void gripBaselines() {
    const auto fixture=gripRehearsalFixture();const auto encoded=encodeGripBaseline(fixture);
    check(decodeGripBaseline(encoded)==fixture,"grip codec round trip");
    rejects([&]{decodeSnapshot(encoded);});rejects([&]{decodeGripBaseline(encodeSnapshot(rehearsalFixture()));});
    rejects([&]{decodeGripBaseline(encoded+"\n");});rejects([&]{decodeGripBaseline(encoded.substr(0,encoded.size()-5));});
    auto tampered=encoded;tampered.replace(tampered.find("grip-only"),9,"full-only");rejects([&]{decodeGripBaseline(tampered);});
    auto physical=fixture;physical.physicalOrigin=true;physical.binding.access=AccessMode::Shared;
    check(decodeGripBaseline(encodeGripBaseline(physical))==physical,"physical grip origin lost");
    for(unsigned scenario=0;scenario<7;++scenario) {
        auto bad=fixture;
        if(scenario==0)bad.schema=2;if(scenario==1)bad.physicalOrigin=true;
        if(scenario==2)bad.binding.access=AccessMode::Shared;if(scenario==3)bad.motor.length=63;
        if(scenario==4)bad.motor.id=5;if(scenario==5)bad.mapping[0]^=1;if(scenario==6)bad.motor.slot=1;
        rejects([&]{validateGripBaseline(bad);});
    }
    FakeIo good(fixture);MemoryTrace tr;Session session(good,tr);check(acquireGripBaseline(session)==fixture,"grip acquisition mismatch");
    const auto plan=gripBaselinePlan();check(plan.size()==14&&good.writes==14&&session.actuatorAttempts()==0,"grip query count");
    for(unsigned i=1;i<=tr.count;++i) {
        FakeIo io(fixture);MemoryTrace trace;trace.failAt=i;Session s(io,trace);
        rejects([&]{acquireGripBaseline(s);});unchangedAfterFault(s,io);
    }
    // A valid CRC must not make an unknown layout or restoration mode acceptable.
    for(auto offset:{0u,23u,25u}) {
        auto bad=fixture;bad.mapping[offset]=3;std::uint32_t crc=0xffffffffu;
        for(auto byte:bad.mapping){crc^=byte;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
        bad.motor.crc32=crc^0xffffffffu;rejects([&]{validateGripBaseline(bad);});
    }
    for(unsigned i=0;i<14;++i) {
        check(good.submissions[i]==fixture.binding.layout.wrap(frame(plan[i].command,plan[i].payload)),"grip exact request order");
        if(plan[i].command==0xa3)check(plan[i].payload[1]==6,"unrelated RAM in grip plan");
    }
    for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved})for(unsigned i=1;i<=14;++i) {
        FakeIo io(fixture);io.failAt=i;io.failStatus=status;MemoryTrace trace;Session s(io,trace);
        rejects([&]{acquireGripBaseline(s);});check(io.writes==i,"grip query failure not stopped");unchangedAfterFault(s,io);
    }
    for(unsigned scenario=0;scenario<12;++scenario) {
        FakeIo io(fixture);MemoryTrace trace;Session s(io,trace);
        io.responseHook=[scenario](auto& f,auto cmd,auto& p) {
            if(scenario==0&&f.writes==13)p[0]^=1; // UID
            if(scenario==1&&f.writes==11)p[0]=1; // slot
            if(scenario==2&&f.writes==14)p[10]^=1; // firmware
            if(scenario==3&&f.writes==12)p[0]^=1; // formats
            if(scenario==4&&f.writes==10)p[6]^=1; // fingerprint
            if(scenario==5&&f.writes==7)p[8]^=1; // block CRC
            if(scenario==6&&f.writes==5)p[4]=63; // refuse before chunks
            if(scenario==7&&f.writes==5)p[2]=5; // wrong RAM echo
            if(scenario==8&&cmd==0xa3)p[1]=4; // error, not ignored
            if(scenario==9&&f.writes==8)p[6]=0; // chunk index
            if(scenario==10&&f.writes==11)p[9]^=1; // fifth config CRC
            if(scenario==11&&f.writes==14)p[24]=0; // capability
        };
        rejects([&]{acquireGripBaseline(s);});unchangedAfterFault(s,io);
        if(scenario==6)check(io.writes==5,"invalid RAM length still read chunks");
    }
    // Rehearsal verifies exactly 14 + 21 + 14, never a full RAM audit.
    for(const auto& baseline:{fixture,physical}) {
        FakeIo io(baseline);MemoryTrace trace;Session s(io,trace);const auto r=rehearseNeutral(s,baseline);
        check(r.complete&&io.writes==49&&s.queryAttempts()==42&&s.actuatorAttempts()==7,"grip rehearsal counts");
        for(const auto& w:io.submissions)if(w[3]==0xa3)check(w[6]==6,"rehearsal queried unrelated RAM");
    }
    for(unsigned i=1;i<=49;++i) {
        FakeIo io(fixture);io.failAt=i;MemoryTrace trace;Session s(io,trace);
        check(!rehearseNeutral(s,fixture).complete&&io.writes==i,"grip lifecycle failure not stopped");unchangedAfterFault(s,io);
    }
    FakeIo locked(physical);locked.pretendPhysical=true;MemoryTrace trace;Session s(locked,trace);
    check(!rehearseNeutral(s,physical).complete&&locked.writes==0&&locked.reads==0,"grip rehearsal opened physical path");
}
void gripApprovals() {
    auto baseline=gripRehearsalFixture();const std::string hash(64,'b');
    GripApproval a{{hash,1000,true,true,true,true,true,true,true,true,true,true},true};
    check(encodeGripApproval(decodeGripApproval(encodeGripApproval(a)))==encodeGripApproval(a),"grip approval codec");
    rejects([&]{decodeApproval(encodeGripApproval(a));});rejects([&]{decodeGripApproval(encodeApproval(a.checkpoints));});
    rejects([&]{decodeGripApproval(encodeGripApproval(a)+"\n");});
    auto scope=encodeGripApproval(a);scope.replace(scope.find("grip-only"),9,"full-only");rejects([&]{decodeGripApproval(scope);});
    rejects([&]{GripNeutralAuthorization::approve(baseline,a,hash,1000);});
    baseline.physicalOrigin=true;baseline.binding.access=AccessMode::Shared;
    auto reduced=a;reduced.reducedAuditAccepted=false;rejects([&]{GripNeutralAuthorization::approve(baseline,reduced,hash,1000);});
    auto auth=GripNeutralAuthorization::approve(baseline,a,hash,1000);rejects([&]{auth.check(999);});rejects([&]{auth.check(1301);});
    rejects([&]{GripNeutralAuthorization::approve(baseline,a,std::string(64,'c'),1000);});
    auto configure=[&](FakeIo& io){io.pretendPhysical=true;io.snapshot.binding=baseline.binding;};
    FakeIo successful(baseline);configure(successful);MemoryTrace trace;Session s(successful,trace);
    const auto r=runAuthorizedGripNeutral(s,auth,[]{return 1000;});
    check(r.complete&&r.deviceStateUncertain&&successful.writes==49&&s.queryAttempts()==42&&s.actuatorAttempts()==7,"authorized grip lifecycle");
    for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved})for(unsigned i=1;i<=49;++i) {
        FakeIo io(baseline);configure(io);io.failAt=i;io.failStatus=status;MemoryTrace tr;Session run(io,tr);
        auto result=runAuthorizedGripNeutral(run,auth,[]{return 1000;});
        check(!result.complete&&io.writes==i,"authorized grip failure did not stop");
        if(i>=25)check(result.deviceStateUncertain,"entry failure uncertainty lost");unchangedAfterFault(run,io);
    }
    for(unsigned i=1;i<=trace.count;++i) {
        FakeIo io(baseline);configure(io);MemoryTrace tr;tr.failAt=i;Session run(io,tr);
        check(!runAuthorizedGripNeutral(run,auth,[]{return 1000;}).complete,"authorized grip trace failure accepted");unchangedAfterFault(run,io);
    }
    for(unsigned scenario=0;scenario<8;++scenario) {
        FakeIo io(baseline);configure(io);MemoryTrace tr;Session run(io,tr);unsigned clocks=0;
        if(scenario==0)io.snapshot.binding.container="replacement";
        if(scenario==1)io.present=false;
        if(scenario==2)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==4)p[0]^=1;};
        if(scenario==3)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==7)p[0]^=1;};
        if(scenario==4)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==0xa1)p[1]^=1;};
        if(scenario==5)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==1)p[10]^=1;};
        auto result=runAuthorizedGripNeutral(run,auth,[&]{++clocks;return scenario==6||(scenario==7&&clocks>1)?1301:1000;});
        check(!result.complete&&run.actuatorAttempts()==0,"grip entered after binding/identity/profile/approval failure");unchangedAfterFault(run,io);
    }
    for(unsigned target=0;target<4;++target) {
        FakeIo io(baseline);configure(io);unsigned modes=0;io.responseHook=[&](auto&,auto cmd,auto& p){if(cmd==0x53&&modes++==target)p[0]=1;};
        MemoryTrace tr;Session run(io,tr);auto result=runAuthorizedGripNeutral(run,auth,[]{return 1000;});
        check(!result.complete&&result.deviceStateUncertain,"grip mode error ignored");unchangedAfterFault(run,io);
    }
}
void restoreOnly() {
    auto baseline=gripRehearsalFixture();const std::string hash(64,'c');
    const GripRestoreApproval approval{hash,1000,true,true,true,true,true,true,true,true,true,true,true,true};
    check(encodeGripRestoreApproval(decodeGripRestoreApproval(encodeGripRestoreApproval(approval)))==encodeGripRestoreApproval(approval),"restore approval codec");
    rejects([&]{decodeGripApproval(encodeGripRestoreApproval(approval));});rejects([&]{decodeApproval(encodeGripRestoreApproval(approval));});
    rejects([&]{decodeGripRestoreApproval(encodeGripApproval({{hash,1000,true,true,true,true,true,true,true,true,true,true},true}));});
    rejects([&]{decodeGripRestoreApproval(encodeGripRestoreApproval(approval)+"\n");});
    rejects([&]{GripRestoreAuthorization::approve(baseline,approval,hash,1000);});
    const auto plan=gripRestoreLeftPlan(baseline);check(plan.size()==1&&plan[0].command==0x53&&plan[0].payload==Bytes({1,0x10,1,0x40}),"restore-only exact payload");
    {FakeIo io(baseline);MemoryTrace trace;Session s(io,trace);check(rehearseGripRestore(s,baseline).complete&&io.writes==29&&s.actuatorAttempts()==1,"restore-only rehearsal count");}
    baseline.physicalOrigin=true;baseline.binding.access=AccessMode::Shared;
    for(auto flag:{&GripRestoreApproval::approved,&GripRestoreApproval::observer,&GripRestoreApproval::powerOffReady,&GripRestoreApproval::normalVibration,
                  &GripRestoreApproval::knownWritersQuiesced,&GripRestoreApproval::directUsb,&GripRestoreApproval::failStopAccepted,&GripRestoreApproval::backgroundRiskAccepted,
                  &GripRestoreApproval::restorationRiskAccepted,&GripRestoreApproval::reducedAuditAccepted,&GripRestoreApproval::powerCycled,&GripRestoreApproval::restoreOnlyAccepted}) {
        auto a=approval;a.*flag=false;rejects([&]{GripRestoreAuthorization::approve(baseline,a,hash,1000);});
    }
    const auto auth=GripRestoreAuthorization::approve(baseline,approval,hash,1000);
    rejects([&]{auth.check(999);});rejects([&]{auth.check(1301);});
    rejects([&]{GripRestoreAuthorization::approve(baseline,approval,std::string(64,'d'),1000);});
    auto configure=[&](FakeIo& io){io.pretendPhysical=true;io.snapshot.binding=baseline.binding;};
    FakeIo good(baseline);configure(good);MemoryTrace trace;Session session(good,trace);
    const auto result=runAuthorizedGripRestore(session,auth,[]{return 1000;});
    check(result.complete&&result.deviceStateUncertain&&session.queryAttempts()==28&&session.actuatorAttempts()==1&&good.writes==29,"restore-only physical simulation counts");
    for(unsigned i=0;i<good.submissions.size();++i) {
        const auto& w=good.submissions[i];check(w[3]!=0x57,"restore-only waveform");
        if(w[3]==0x53)check(i==14&&w==baseline.binding.layout.wrap(frame(plan[0].command,plan[0].payload)),"restore-only unexpected mode command");
        if(w[3]==0xa3)check(w[6]==6,"restore-only unrelated RAM");
    }
    for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved})for(unsigned n=1;n<=29;++n) {
        FakeIo io(baseline);configure(io);io.failAt=n;io.failStatus=status;MemoryTrace tr;Session s(io,tr);
        auto r=runAuthorizedGripRestore(s,auth,[]{return 1000;});check(!r.complete&&io.writes==n,"restore-only failure did not stop");
        if(n>=15)check(r.deviceStateUncertain,"restore-only uncertainty missing");unchangedAfterFault(s,io);
    }
    for(unsigned n=1;n<=trace.count;++n) {
        FakeIo io(baseline);configure(io);MemoryTrace tr;tr.failAt=n;Session s(io,tr);
        check(!runAuthorizedGripRestore(s,auth,[]{return 1000;}).complete,"restore-only trace fault accepted");unchangedAfterFault(s,io);
    }
    for(unsigned scenario=0;scenario<10;++scenario) {
        FakeIo io(baseline);configure(io);MemoryTrace tr;Session s(io,tr);unsigned clocks=0;
        if(scenario==0)io.rawResponseHook=[](auto&,auto& wire){if(wire[3]==0x53)wire=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");};
        if(scenario==1)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==0x53)p[0]=1;};
        if(scenario==2)io.snapshot.binding.container="replacement";
        if(scenario==3)io.present=false;
        if(scenario==4)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==4)p[0]^=1;};
        if(scenario==5)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==0xa1)p[1]^=1;};
        if(scenario==6)io.responseHook=[](auto&,auto cmd,auto& p){if(cmd==1)p[10]^=1;};
        if(scenario==7)io.writeDelay=Time(600001);
        const auto r=runAuthorizedGripRestore(s,auth,[&]{++clocks;return scenario==8||(scenario==9&&clocks>1)?1301:1000;});
        check(!r.complete,"restore-only invalid scenario accepted");unchangedAfterFault(s,io);
        if(scenario<2)check(io.writes==15&&s.actuatorAttempts()==1,"error reply followed by traffic");
        else check(s.actuatorAttempts()==0,"restore proceeded after preflight failure");
        if(scenario==0)check(r.failure=="GPA6 zero-count envelope error 1","recorded restore envelope misclassified");
    }
    {FakeIo io(baseline);configure(io);MemoryTrace tr;Session s(io,tr);check(!rehearseGripRestore(s,baseline).complete&&io.writes==0,"physical restore rehearsal allowed");}
}
void restoreObservation() {
    auto baseline=gripRehearsalFixture();baseline.physicalOrigin=true;baseline.binding.access=AccessMode::Shared;
    const std::string hash(64,'e');
    GripRestoreApproval approval{hash,1000,true,true,true,true,true,true,true,true,true,true,true,true};
    rejects([&]{GripRestoreAuthorization::approve(baseline,approval,hash,1000,true);});
    approval.observeRepliesAccepted=true;
    rejects([&]{GripRestoreAuthorization::approve(baseline,approval,hash,1000);});
    check(decodeGripRestoreApproval(encodeGripRestoreApproval(approval)).observeRepliesAccepted,"observation token lost policy");
    const auto auth=GripRestoreAuthorization::approve(baseline,approval,hash,1000,true);
    rejects([&]{auth.check(1301);});
    const auto zero=unhex("005aa5530000010000000000000000000000000000000000000000000000000054");
    const auto ack=unhex("005aa5530100000000000000000000000000000000000000000000000000000054");
    unsigned goodTraceCount=0;
    for(unsigned scenario=0;scenario<12;++scenario) {
        FakeIo io(baseline);io.pretendPhysical=true;io.snapshot.binding=baseline.binding;
        MemoryTrace tr;Session s(io,tr);unsigned modeReads=0;
        io.rawResponseHook=[&](auto&,auto& wire){if(wire[3]==0x53&&scenario!=3)wire=zero;};
        io.readHook=[&](auto& fake,Time deadline,bool poll)->std::optional<IoResult> {
            if(fake.writes!=15||poll)return {};
            if(++modeReads==1)return {};
            if(scenario==1&&modeReads==2)fake.incoming.push_back(ack);
            if(scenario==2)fake.incoming.push_back(modeReads%2?zero:ack);
            if(scenario==4)return IoResult{Completion::Unresolved,{},0,123};
            if(scenario==5)return IoResult{Completion::Failed,{},0,123};
            if(scenario==6)return IoResult{Completion::Complete,Bytes(3),3};
            if(scenario==7){fake.clock-=Time(1);return IoResult{Completion::Complete,ack,33};}
            if(scenario==8){fake.clock=deadline+Time(1);return IoResult{Completion::Complete,ack,33};}
            if(scenario==9){fake.present=false;return IoResult{Completion::Complete,ack,33};}
            if(scenario==10)return IoResult{Completion::Timeout,{}};
            if(scenario==11&&modeReads==2)fake.incoming.push_back(Bytes(33,0xff));
            return {};
        };
        auto result=runAuthorizedGripRestore(s,auth,[]{return 1000;});
        check(!result.complete&&result.deviceStateUncertain&&s.failed(),"observation promoted to restoration success");
        check(io.writes==15&&s.queryAttempts()==14&&s.actuatorAttempts()==1,"observation wrote after restore");
        const auto& obs=s.observation();
        check(obs.started,"observation did not start after full reply");
        check(obs.complete==(scenario<4||scenario==11),"observation fault/capture classification");
        if(scenario==1)check(obs.reports==1&&obs.firstReply=="GPA6 zero-count envelope error 1","late ACK erased initial error");
        if(scenario==2)check(obs.reports==32&&obs.stop=="report_limit"&&modeReads==33,"receive flood exceeded limit");
        if(scenario==0)check(obs.reports==0&&obs.elapsed==Time(500000)&&obs.stop=="deadline","silence not bounded");
        if(scenario==3)check(obs.firstReply=="normal_mode_ack","first ACK classification");
        if(scenario==11)check(obs.reports==1,"malformed raw tail not retained");
        if(scenario==1)goodTraceCount=tr.count;
        unchangedAfterFault(s,io);
    }
    for(unsigned n=1;n<=15;++n)for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved}) {
        FakeIo io(baseline);io.pretendPhysical=true;io.snapshot.binding=baseline.binding;io.failAt=n;io.failStatus=status;
        MemoryTrace tr;Session s(io,tr);auto result=runAuthorizedGripRestore(s,auth,[]{return 1000;});
        check(!result.complete&&io.writes==n&&!s.observation().started,"follow-up after failed preflight/write");unchangedAfterFault(s,io);
    }
    for(unsigned n=1;n<=goodTraceCount;++n) {
        FakeIo io(baseline);io.pretendPhysical=true;io.snapshot.binding=baseline.binding;MemoryTrace tr;tr.failAt=n;Session s(io,tr);
        auto result=runAuthorizedGripRestore(s,auth,[]{return 1000;});
        check(!result.complete&&io.writes<=15,"trace failure permitted extra writes");unchangedAfterFault(s,io);
    }
    {FakeIo io(baseline);io.pretendPhysical=true;MemoryTrace tr;Session s(io,tr);check(!rehearseGripRestore(s,baseline,true).complete&&io.writes==0,"physical observation rehearsal allowed");}
    {FakeIo io(baseline);MemoryTrace tr;Session s(io,tr);rehearseGripRestore(s,baseline,true);check(s.observation().complete&&io.writes==15,"observation rehearsal failed");}
}
void restoreConsole() {
    const std::string hash(64,'a');
    std::istringstream input("YES\nYES\n"+hash+"\n");std::ostringstream output;
    auto a=collectRestoreConfirmations(input,output,hash);
    check(a.approved&&a.powerCycled&&a.directUsb&&a.normalVibration&&a.observer&&
        a.powerOffReady&&a.knownWritersQuiesced&&a.backgroundRiskAccepted&&
        a.restorationRiskAccepted&&a.reducedAuditAccepted&&a.failStopAccepted&&
        a.restoreOnlyAccepted,"grouped approval omitted a checkpoint");
    check(a.manifestHash==hash&&a.confirmedUnixSeconds==0,"collector changed binding or timestamp");
    const auto text=output.str();std::size_t pos=0;unsigned prompts=0;
    while((pos=text.find("Type YES",pos))!=std::string::npos){++prompts;pos+=8;}
    check(prompts==2&&text.find("[3/3]")!=std::string::npos,"restore approval must have only three prompts");
    for(const auto* warning:{"no streaming entry","error 1","RAM 1/4/5","ALL traffic","Shared-access"})
        check(text.find(warning)!=std::string::npos,"grouped approval lost a disclosure");
    for(const auto& answers:std::vector<std::string>{"","NO\n","yes\n","YES\n","YES\nNO\n",
        "YES\nYES\n","YES\nYES\nwrong\n","YES\nYES\n"+hash+" \n"}) {
        std::istringstream denied(answers);std::ostringstream log;
        rejects([&]{collectRestoreConfirmations(denied,log,hash);});
    }
    std::istringstream cancel("NO\nYES\n"+hash+"\n");std::ostringstream log;
    rejects([&]{collectRestoreConfirmations(cancel,log,hash);});
    check(log.str().find("[2/3]")==std::string::npos,"cancellation continued prompting");
    std::istringstream observeInput("YES\nYES\n"+hash+"\n");std::ostringstream observeOutput;
    check(collectRestoreConfirmations(observeInput,observeOutput,hash,true).observeRepliesAccepted,"observation consent missing");
    check(observeOutput.str().find("500 ms / 32")!=std::string::npos&&observeOutput.str().find("even after an unexpected reply")!=std::string::npos,"observation policy undisclosed");
}
void silentLifecycle() {
    auto baseline=gripRehearsalFixture();const auto plan=gripLifecyclePlan(baseline);
    const auto zero=unhex(silent_capture::reports[10].windows);
    unsigned requests=0,anomalies=0;long long previous=0;
    for(const auto& r:silent_capture::reports) {
        const auto body=unhex(r.body),wire=unhex(r.windows);
        check(body.size()==32&&wire.size()==33&&Layout{}.unwrap(wire)==body,"capture framing changed");
        check(r.relativeUs>previous,"capture timing order changed");previous=r.relativeUs;
        if(!r.inbound){check(Layout{}.wrap(frame(plan[requests].command,plan[requests].payload))==wire,"capture request changed");++requests;}
        else {
            const bool anomaly=r.frame==429||r.frame==431;anomalies+=anomaly;
            check(classifyModeReply(body)==(anomaly?ModeReply::CapturedZeroCountValue1:ModeReply::NormalSuccessAck),"capture reply classification");
            if(anomaly)rejects([&]{replyPayload(body,0x53,1);});
        }
    }
    check(requests==4&&anomalies==2,"capture fixture incomplete");
    for(unsigned byte=0;byte<32;++byte){auto b=Layout{}.unwrap(zero);b[byte]^=0x80;check(classifyModeReply(b)==ModeReply::InvalidUnexpected,"nonexact anomaly admitted");}
    check(classifyModeReply(zero)==ModeReply::InvalidUnexpected,"Windows ID stripped implicitly");
    unsigned goodEvents=0;
    for(unsigned mask=0;mask<4;++mask) {
        FakeIo io(baseline);MemoryTrace tr;Session s(io,tr);
        io.rawResponseHook=[&](auto& fake,auto& wire){if((fake.writes==17&&(mask&1))||(fake.writes==18&&(mask&2)))wire=zero;};
        auto r=rehearseGripLifecycle(s,baseline);
        check(r.sequenceComplete&&r.postflightMatches&&r.failure.empty()&&!r.restorationVerified&&r.deviceStateUncertain,"lifecycle diagnostic result");
        check(io.writes==32&&s.queryAttempts()==28&&s.actuatorAttempts()==4,"lifecycle counts");
        for(unsigned i=0;i<2;++i)check(r.restoreReplies[i]==((mask&(1<<i))?ModeReply::CapturedZeroCountValue1:ModeReply::NormalSuccessAck),"restore evidence lost");
        for(unsigned i=0;i<32;++i){const auto expected=i<14?gripBaselinePlan()[i]:i<18?plan[i-14]:gripBaselinePlan()[i-18];check(io.submissions[i]==baseline.binding.layout.wrap(frame(expected.command,expected.payload)),"lifecycle sequence mismatch");}
        goodEvents=tr.count;
    }
    for(unsigned n=1;n<=32;++n)for(auto status:{Completion::Failed,Completion::Timeout,Completion::Unresolved}) {
        FakeIo io(baseline);io.failAt=n;io.failStatus=status;MemoryTrace tr;Session s(io,tr);
        auto r=rehearseGripLifecycle(s,baseline);check(!r.failure.empty()&&io.writes==n,"lifecycle write fault continued");unchangedAfterFault(s,io);
    }
    for(unsigned n=1;n<=goodEvents;++n) {
        FakeIo io(baseline);MemoryTrace tr;tr.failAt=n;Session s(io,tr);
        auto r=rehearseGripLifecycle(s,baseline);check(!r.failure.empty(),"lifecycle trace failure accepted");unchangedAfterFault(s,io);
    }
    for(unsigned step=15;step<=18;++step)for(unsigned scenario=0;scenario<13;++scenario) {
        FakeIo io(baseline);MemoryTrace tr;Session s(io,tr);
        io.rawResponseHook=[&](auto& fake,auto& wire){if(fake.writes!=step)return;
            if(scenario==0)wire=zero;
            if(scenario==1)wire[32]^=1;
            if(scenario==2){wire[6]=1;wire[32]=0x55;} // valid normal nonzero status
            if(scenario==3){wire=zero;wire[6]=2;wire[32]=0x55;} // unknown zero-count value
            if(scenario==4){wire[3]=4;wire[32]=5;}
            if(scenario==5)fake.present=false;
            if(scenario==10)wire[0]=1;
            if(scenario==11)wire.pop_back();
        };
        io.readHook=[&](auto& fake,Time deadline,bool poll)->std::optional<IoResult>{
            if(poll&&fake.writes==step-1&&scenario==6)return IoResult{Completion::Complete,zero,33};
            if(poll&&fake.writes==step-1&&scenario==12)fake.shortWrite=true;
            if(!poll&&fake.writes==step){
                if(scenario==7){fake.clock=deadline;return IoResult{Completion::Complete,zero,33};}
                if(scenario==8)return IoResult{Completion::Timeout,{},0};
                if(scenario==9)return IoResult{Completion::Complete,zero,32};
            }return {};
        };
        auto r=rehearseGripLifecycle(s,baseline);
        if(scenario==0&&step>=17)check(r.failure.empty(),"approved captured envelope rejected");
        else {
            check(!r.failure.empty(),"lifecycle invalid reply/fault accepted");unchangedAfterFault(s,io);
            if(step>=17&&(scenario==10||scenario==11))check(r.restoreReplies[step-17]==ModeReply::InvalidUnexpected,"malformed Windows framing not classified");
        }
    }
    for(bool post:{false,true})for(auto cmd:{1,4,7,0xa1,0xa3}) {
        FakeIo io(baseline);MemoryTrace tr;Session s(io,tr);
        io.responseHook=[&](auto& fake,auto command,auto& p){if(command==cmd&&(!post||fake.writes>18))p[cmd==1?10:0]^=1;};
        auto r=rehearseGripLifecycle(s,baseline);check(!r.failure.empty()&&!r.postflightMatches,"changed baseline admitted");if(!post)check(s.actuatorAttempts()==0,"actuation after baseline change");unchangedAfterFault(s,io);
    }
    {FakeIo io(baseline);io.pretendPhysical=true;MemoryTrace tr;Session s(io,tr);check(!rehearseGripLifecycle(s,baseline).failure.empty()&&io.writes==0,"physical rehearsal admitted");}
    auto physical=baseline;physical.physicalOrigin=true;physical.binding.access=AccessMode::Shared;
    const std::string hash(64,'a');std::istringstream input("YES\nYES\n"+hash+"\n");std::ostringstream output;
    auto approval=collectLifecycleConfirmations(input,output,hash);approval.checkpoints.confirmedUnixSeconds=1000;
    check(output.str().find("[3/3]")!=std::string::npos&&output.str().find("UNVERIFIED")!=std::string::npos,"lifecycle prompt disclosures");
    const auto encoded=encodeGripLifecycleApproval(approval);check(encodeGripLifecycleApproval(decodeGripLifecycleApproval(encoded))==encoded,"lifecycle token roundtrip");
    rejects([&]{decodeGripRestoreApproval(encoded);});rejects([&]{decodeGripLifecycleApproval(encodeGripRestoreApproval(approval.checkpoints));});
    rejects([&]{decodeGripApproval(encoded);});rejects([&]{decodeGripLifecycleApproval(encodeGripApproval({}));});
    auto auth=GripLifecycleAuthorization::approve(physical,approval,hash,1000);
    rejects([&]{auth.check(1301);});rejects([&]{auth.check(999);});rejects([&]{GripLifecycleAuthorization::approve(physical,approval,std::string(64,'b'),1000);});
    auth.consume();auto copy=auth;rejects([&]{copy.consume();});
    for(const auto& answers:{std::string("NO\n"),std::string("YES\nNO\n"),std::string("YES\nYES\nwrong\n")}){std::istringstream in(answers);rejects([&]{collectLifecycleConfirmations(in,output,hash);});}
    {FakeIo io(physical);io.pretendPhysical=true;io.snapshot.binding=physical.binding;MemoryTrace tr;Session s(io,tr);auto r=runAuthorizedGripLifecycle(s,auth,[]{return 1000;});check(r.failure.empty()&&r.postflightMatches,"authorized lifecycle simulation failed");}
    {FakeIo io(physical);io.pretendPhysical=true;io.snapshot.binding=physical.binding;MemoryTrace tr;Session s(io,tr);unsigned calls=0;
        auto r=runAuthorizedGripLifecycle(s,auth,[&]{return ++calls>1?1301:1000;});check(!r.failure.empty()&&s.actuatorAttempts()==0,"expired approval admitted entry");unchangedAfterFault(s,io);}
}
void evidence() {
    const auto base=std::filesystem::temp_directory_path()/("asb-apex6-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(base);
    {Evidence e(base/"ok");e.record(Time(0),"test",Bytes{0,128,255});check(e.finish(),"trace finish");}
    {Evidence e(base/"disk",{},[]{throw std::runtime_error("injected disk full");});e.record(Time(0),"test");check(!e.finish(),"disk failure hidden");}
    {Evidence e(base/"budget",{200,200,100});rejects([&]{e.record(Time(0),std::string(120,'x'));});check(!e.healthy(),"storage bound not latched");}
    {std::promise<void> release;auto ready=release.get_future().share();Evidence e(base/"queue",{8192,200,64},[ready]{ready.wait();});
        e.record(Time(0),"one");bool failed=false;try{for(unsigned i=0;i<10;++i)e.record(Time(0),"fill");}catch(const ProtocolError&){failed=true;}
        release.set_value();check(failed&&!e.finish(),"bounded queue overflow not latched");}
    // Keep tiny test artifacts for diagnosis; never recursively delete a computed directory.
    std::cout<<"Evidence fixtures: "<<base.string()<<'\n';
}
}
int main(){try{codecAndSnapshots();successAndEveryFailure();faults();approvals();gripBaselines();gripApprovals();restoreOnly();restoreObservation();restoreConsole();silentLifecycle();evidence();std::cout<<checks<<" experiment checks passed; zero physical calls\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
