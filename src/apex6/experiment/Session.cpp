// SPDX-FileCopyrightText: 2026 Mikalai Kaliaha
// SPDX-License-Identifier: MIT
#include "apex6/experiment/Session.h"
#include <algorithm>
#include <charconv>
#include <sstream>

namespace asb::apex6::experiment {
namespace {
constexpr std::array<std::uint8_t, 4> ids{1,4,5,6};
std::uint16_t le16(std::span<const std::uint8_t> p, std::size_t n) { return p[n] | (p[n+1] << 8); }
Request query(std::uint8_t cmd) {
    switch(cmd) {
    case 1: return {cmd,{},25}; case 4: return {cmd,{},16};
    case 7: return {cmd,{},6}; case 0xa1: return {cmd,{},11};
    default: throw ProtocolError("unsupported query");
    }
}
Request ramQuery(std::uint8_t id) { return {0xa3,{1,id},10,true}; }
Request chunk(std::uint8_t id, std::uint8_t index) { return {0xa3,{0,id,index,16},24,true,true}; }
void equal(bool match, const char* reason) { if (!match) throw ProtocolError(reason); }
Bytes infoPayload(const Identity& info) {
    Bytes p(25); p[0]=info.deviceType; p[1]=info.connection; p[24]=info.features;
    for(std::size_t i=0;i<7;++i) { p[10+2*i]=static_cast<std::uint8_t>(info.firmware[i]>>8); p[11+2*i]=static_cast<std::uint8_t>(info.firmware[i]); }
    return p;
}
void validateRequest(const Request& r, bool allowNeutral) {
    if(r.command==1 || r.command==4 || r.command==7 || r.command==0xa1) {
        equal(r==query(r.command),"query contract mismatch"); return;
    }
    if(r.command==0xa3 && r.payload.size()>=2 && std::find(ids.begin(),ids.end(),r.payload[1])!=ids.end()) {
        if(r.payload.size()==2 && r.payload[0]==1) { equal(r==ramQuery(r.payload[1]),"RAM query contract mismatch"); return; }
        if(r.payload.size()==4 && r.payload[0]==0 && r.payload[3]==16) { equal(r==chunk(r.payload[1],r.payload[2]),"RAM chunk contract mismatch"); return; }
    }
    if(allowNeutral && (r.command==0x53 || r.command==0x57)) return; // exact plan also required
    throw ProtocolError("request not permitted");
}
}
std::string accessName(AccessMode mode) {
    switch(mode){case AccessMode::Unknown:return "unknown";case AccessMode::Exclusive:return "exclusive";case AccessMode::Shared:return "shared";case AccessMode::Synthetic:return "synthetic";}
    throw ProtocolError("invalid access mode");
}
AccessMode parseAccess(const std::string& value) {
    if(value=="exclusive")return AccessMode::Exclusive;if(value=="shared")return AccessMode::Shared;
    if(value=="synthetic")return AccessMode::Synthetic;if(value=="unknown")return AccessMode::Unknown;
    throw ProtocolError("invalid access mode");
}
void Layout::validate() const {
    if(inputLength<33 || inputLength>65 || outputLength<33 || outputLength>65)
        throw ProtocolError("unsupported Windows report length (ID included)");
}
Bytes Layout::wrap(const Frame& f) const {
    validate(); Bytes wire(outputLength); wire[0]=outputId; std::copy(f.begin(),f.end(),wire.begin()+1); return wire;
}
Bytes Layout::unwrap(std::span<const std::uint8_t> wire) const {
    validate();
    if(wire.size()!=inputLength || wire[0]!=inputId) throw ProtocolError("Windows HID report ID/length mismatch");
    return Bytes(wire.begin()+1,wire.end());
}
Identity identity(std::span<const std::uint8_t> p) {
    if(p.size()!=25) throw ProtocolError("invalid info payload length");
    Identity result{p[0],p[1],p[24],{}};
    for(std::size_t i=0;i<7;++i) result.firmware[i]=static_cast<std::uint16_t>((p[10+2*i]<<8)|p[11+2*i]);
    if(result.deviceType!=150 || !(result.features&0x80)) throw ProtocolError("not Apex6Pro with haptic grips");
    return result;
}
Uid uid(std::span<const std::uint8_t> p) {
    if(p.size()!=16 || std::all_of(p.begin(),p.end(),[](auto b){return b==0;}) || std::all_of(p.begin(),p.end(),[](auto b){return b==255;}))
        throw ProtocolError("invalid unit UID");
    Uid out{}; std::copy(p.begin(),p.end(),out.begin()); return out;
}
std::array<std::uint16_t,3> versions(std::span<const std::uint8_t> p) {
    if(p.size()!=6) throw ProtocolError("invalid format version payload");
    return {le16(p,0),le16(p,2),le16(p,4)};
}
std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char digits[]="0123456789abcdef"; std::string out;
    for(auto b:bytes) {out+=digits[b>>4]; out+=digits[b&15];} return out;
}
Bytes unhex(const std::string& text) {
    if(text.size()%2 || text.size()>65536) throw ProtocolError("invalid hex length");
    Bytes out;
    auto nibble=[](char c)->unsigned {if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; throw ProtocolError("noncanonical hex");};
    for(std::size_t i=0;i<text.size();i+=2) out.push_back(static_cast<std::uint8_t>((nibble(text[i])<<4)|nibble(text[i+1])));
    return out;
}
void validateSnapshot(const Snapshot& s) {
    if((s.schema!=1&&s.schema!=2)||(s.schema==1&&s.binding.access!=AccessMode::Unknown)||(s.schema==2&&s.binding.access==AccessMode::Unknown))throw ProtocolError("snapshot schema/acquisition mismatch");
    accessName(s.binding.access);
    s.binding.layout.validate(); identity(infoPayload(s.info)); uid(s.unit);
    if(s.binding.path.empty() || s.binding.instance.empty() || s.binding.container.empty() || s.binding.descriptorSignature.empty()) throw ProtocolError("incomplete interface binding");
    for(const auto* text:{&s.binding.path,&s.binding.instance,&s.binding.container,&s.binding.descriptorSignature})
        if(text->size()>16384||text->find('\0')!=std::string::npos)throw ProtocolError("invalid interface binding text");
    if(s.config.slot>3) throw ProtocolError("invalid snapshot slot");
    for(std::size_t n=0;n<4;++n) {
        auto f=s.fingerprints[n];
        if(f.id!=ids[n] || f.slot!=s.config.slot || f.length==0 || f.length!=s.blocks[n].size()) throw ProtocolError("invalid snapshot RAM metadata");
        std::vector<Bytes> chunks;
        for(std::size_t i=0;i<s.blocks[n].size();i+=16) {
            auto count=std::min<std::size_t>(16,s.blocks[n].size()-i);
            Bytes p{0,0,f.id,f.slot,static_cast<std::uint8_t>(f.length),static_cast<std::uint8_t>(f.length>>8),static_cast<std::uint8_t>(i/16),static_cast<std::uint8_t>(count)};
            p.insert(p.end(),s.blocks[n].begin()+i,s.blocks[n].begin()+i+count); chunks.push_back(p);
        }
        assembleRam(f,f,chunks);
    }
    gripRestore(s.blocks[3]);
}
std::string encodeSnapshot(const Snapshot& s) {
    validateSnapshot(s);
    auto texthex=[](const std::string& value){return hex({reinterpret_cast<const std::uint8_t*>(value.data()),value.size()});};
    std::ostringstream out;
    out<<(s.schema==1?"ASB_APEX6_SNAPSHOT_V1\n":"ASB_APEX6_SNAPSHOT_V2\n");
    if(s.schema==2)out<<accessName(s.binding.access)<<'\n';
    out<<texthex(s.binding.path)<<'\n'<<texthex(s.binding.instance)<<'\n'<<texthex(s.binding.container)<<'\n'<<texthex(s.binding.descriptorSignature)<<'\n';
    auto l=s.binding.layout;
    out<<unsigned(l.inputId)<<' '<<unsigned(l.outputId)<<' '<<l.inputLength<<' '<<l.outputLength<<'\n';
    out<<hex(infoPayload(s.info))<<'\n'<<hex(s.unit)<<'\n';
    for(auto v:s.formats) out<<v<<' '; out<<'\n'<<unsigned(s.config.slot)<<' ';
    for(auto v:s.config.crc16) out<<v<<' '; out<<'\n';
    for(std::size_t i=0;i<4;++i) {const auto& f=s.fingerprints[i];out<<unsigned(f.id)<<' '<<unsigned(f.slot)<<' '<<f.length<<' '<<f.crc32<<' '<<hex(s.blocks[i])<<'\n';}
    return out.str();
}
Snapshot decodeSnapshot(const std::string& text) {
    if(text.size()>65536) throw ProtocolError("snapshot file too large");
    std::istringstream in(text); std::string token; Snapshot s;
    auto word=[&](){std::string v; if(!(in>>v))throw ProtocolError("truncated snapshot");return v;};
    auto number=[&](std::uint64_t max){auto v=word();std::uint64_t n=0;auto r=std::from_chars(v.data(),v.data()+v.size(),n);if(r.ec!=std::errc{}||r.ptr!=v.data()+v.size()||n>max)throw ProtocolError("invalid snapshot number");return n;};
    const auto schema=word();equal(schema=="ASB_APEX6_SNAPSHOT_V1"||schema=="ASB_APEX6_SNAPSHOT_V2","unknown snapshot schema");
    s.schema=schema=="ASB_APEX6_SNAPSHOT_V1"?1:2;if(s.schema==2)s.binding.access=parseAccess(word());
    auto readtext=[&](){auto b=unhex(word());return std::string(b.begin(),b.end());};
    s.binding.path=readtext();s.binding.instance=readtext();s.binding.container=readtext();s.binding.descriptorSignature=readtext();
    s.binding.layout={static_cast<std::uint8_t>(number(255)),static_cast<std::uint8_t>(number(255)),static_cast<std::uint16_t>(number(65)),static_cast<std::uint16_t>(number(65))};
    s.info=identity(unhex(word()));s.unit=uid(unhex(word()));
    for(auto& v:s.formats)v=static_cast<std::uint16_t>(number(65535));
    s.config.slot=static_cast<std::uint8_t>(number(3));for(auto& v:s.config.crc16)v=static_cast<std::uint16_t>(number(65535));
    for(std::size_t i=0;i<4;++i) {auto& f=s.fingerprints[i]; f={static_cast<std::uint8_t>(number(255)),static_cast<std::uint8_t>(number(3)),static_cast<std::uint16_t>(number(4096)),static_cast<std::uint32_t>(number(0xffffffffu))};s.blocks[i]=unhex(word());}
    equal(encodeSnapshot(s)==text,"noncanonical or trailing snapshot data");return s;
}
std::vector<Request> snapshotPlan(const Snapshot& s) {
    validateSnapshot(s);
    std::vector<Request> p{query(1),query(4),query(1),query(7),query(0xa1)};
    for(auto id:ids)p.push_back(ramQuery(id));
    for(std::size_t n=0;n<4;++n) {p.push_back(ramQuery(ids[n]));for(unsigned i=0;i<(s.blocks[n].size()+15)/16;++i)p.push_back(chunk(ids[n],static_cast<std::uint8_t>(i)));p.push_back(ramQuery(ids[n]));}
    for(auto id:ids)p.push_back(ramQuery(id));
    p.push_back(query(0xa1));p.push_back(query(7));p.push_back(query(4));p.push_back(query(1));
    if(p.size()>128)throw ProtocolError("snapshot exceeds 128 attempts");return p;
}
namespace {
std::vector<Request> gripNeutralPlan(const Bytes& mapping) {
    std::vector<Request> p{query(1),query(1),query(4),query(7),query(0xa1)};
    for(unsigned i=0;i<4;++i)p.push_back(chunk(6,static_cast<std::uint8_t>(i)));
    p.push_back(query(0xa1));p.push_back({0x53,{1,0x12,2,0x40,0},1});p.push_back(query(4));p.push_back(query(0xa1));
    const auto neutral=gripWaveform({}), disabled=waveform({}, {false,false,false});
    const Request n{0x57,Bytes(neutral.begin()+4,neutral.begin()+29),0};
    p.push_back(n);p.push_back(query(4));p.push_back(n);p.push_back({0x57,Bytes(disabled.begin()+4,disabled.begin()+29),0});
    p.push_back({0x53,{1,0x12,0},1});p.push_back(query(0xa1));
    for(auto r:gripRestore(mapping)) {Bytes b{1,r.target,r.mode};b.insert(b.end(),r.parameters.begin(),r.parameters.end());p.push_back({0x53,b,1});}
    return p;
}
}
std::vector<Request> neutralPlan(const Snapshot& s) {
    validateSnapshot(s);return gripNeutralPlan(s.blocks[3]);
}
void validateGripBaseline(const GripBaseline& s) {
    equal(s.schema==1,"unknown grip baseline schema");
    equal(s.physicalOrigin?s.binding.access==AccessMode::Shared:s.binding.access==AccessMode::Synthetic,
          "grip baseline origin/access mismatch");
    s.binding.layout.validate();identity(infoPayload(s.info));uid(s.unit);
    for(const auto* value:{&s.binding.path,&s.binding.instance,&s.binding.container,&s.binding.descriptorSignature})
        equal(!value->empty()&&value->size()<=16384&&value->find('\0')==std::string::npos,"invalid grip interface binding");
    equal(s.config.slot<=3&&s.motor.id==6&&s.motor.slot==s.config.slot&&s.motor.length==64&&s.mapping.size()==64,
          "invalid grip RAM metadata");
    std::vector<Bytes> chunks;
    for(unsigned i=0;i<4;++i) {
        Bytes p{0,0,6,s.config.slot,64,0,static_cast<std::uint8_t>(i),16};
        p.insert(p.end(),s.mapping.begin()+16*i,s.mapping.begin()+16*(i+1));chunks.push_back(p);
    }
    assembleRam(s.motor,s.motor,chunks);gripRestore(s.mapping);
}
std::string encodeGripBaseline(const GripBaseline& s) {
    validateGripBaseline(s);
    auto texthex=[](const std::string& value){return hex({reinterpret_cast<const std::uint8_t*>(value.data()),value.size()});};
    std::ostringstream out;out<<"ASB_APEX6_GRIP_BASELINE_V1\ngrip-only\n"<<(s.physicalOrigin?"physical":"synthetic")<<'\n'<<accessName(s.binding.access)<<'\n';
    for(const auto* value:{&s.binding.path,&s.binding.instance,&s.binding.container,&s.binding.descriptorSignature})out<<texthex(*value)<<'\n';
    const auto l=s.binding.layout;out<<unsigned(l.inputId)<<' '<<unsigned(l.outputId)<<' '<<l.inputLength<<' '<<l.outputLength<<'\n';
    out<<hex(infoPayload(s.info))<<'\n'<<hex(s.unit)<<'\n';
    for(auto v:s.formats)out<<v<<' ';out<<'\n'<<unsigned(s.config.slot)<<' ';
    for(auto v:s.config.crc16)out<<v<<' ';out<<'\n';
    out<<unsigned(s.motor.id)<<' '<<unsigned(s.motor.slot)<<' '<<s.motor.length<<' '<<s.motor.crc32<<' '<<hex(s.mapping)<<'\n';
    // Derived entries are serialized too, but decoding must regenerate them.
    for(const auto& r:gripRestore(s.mapping))out<<unsigned(r.target)<<' '<<unsigned(r.mode)<<' '<<hex(r.parameters)<<'\n';
    auto text=out.str();equal(text.size()<=65536,"grip baseline too large");return text;
}
GripBaseline decodeGripBaseline(const std::string& text) {
    equal(text.size()<=65536,"grip baseline too large");std::istringstream in(text);GripBaseline s;
    auto word=[&](){std::string v;if(!(in>>v))throw ProtocolError("truncated grip baseline");return v;};
    auto number=[&](std::uint64_t max){auto v=word();std::uint64_t n=0;auto r=std::from_chars(v.data(),v.data()+v.size(),n);equal(r.ec==std::errc{}&&r.ptr==v.data()+v.size()&&n<=max,"invalid grip baseline number");return n;};
    equal(word()=="ASB_APEX6_GRIP_BASELINE_V1","unknown grip baseline schema");equal(word()=="grip-only","invalid grip baseline scope");
    const auto origin=word();equal(origin=="physical"||origin=="synthetic","unknown grip baseline origin");s.physicalOrigin=origin=="physical";s.binding.access=parseAccess(word());
    for(auto* value:{&s.binding.path,&s.binding.instance,&s.binding.container,&s.binding.descriptorSignature}){auto b=unhex(word());value->assign(b.begin(),b.end());}
    s.binding.layout={static_cast<std::uint8_t>(number(255)),static_cast<std::uint8_t>(number(255)),static_cast<std::uint16_t>(number(65)),static_cast<std::uint16_t>(number(65))};
    s.info=identity(unhex(word()));s.unit=uid(unhex(word()));
    for(auto& v:s.formats)v=static_cast<std::uint16_t>(number(65535));
    s.config.slot=static_cast<std::uint8_t>(number(3));for(auto& v:s.config.crc16)v=static_cast<std::uint16_t>(number(65535));
    s.motor={static_cast<std::uint8_t>(number(255)),static_cast<std::uint8_t>(number(3)),static_cast<std::uint16_t>(number(64)),static_cast<std::uint32_t>(number(0xffffffffu))};s.mapping=unhex(word());
    equal(encodeGripBaseline(s)==text,"noncanonical/tampered grip baseline or restoration entries");return s;
}
std::vector<Request> gripBaselinePlan() {
    std::vector<Request> p{query(1),query(4),query(7),query(0xa1),ramQuery(6)};
    for(unsigned i=0;i<4;++i)p.push_back(chunk(6,static_cast<std::uint8_t>(i)));
    for(auto r:{ramQuery(6),query(0xa1),query(7),query(4),query(1)})p.push_back(r);
    return p;
}
std::vector<Request> neutralPlan(const GripBaseline& s) {validateGripBaseline(s);return gripNeutralPlan(s.mapping);}
std::vector<Request> gripRestoreLeftPlan(const GripBaseline& s) {
    validateGripBaseline(s);const auto left=gripRestore(s.mapping)[0];
    Bytes payload{1,left.target,left.mode};payload.insert(payload.end(),left.parameters.begin(),left.parameters.end());
    return {{0x53,payload,1}};
}
std::vector<Request> gripLifecyclePlan(const GripBaseline& s) {
    validateGripBaseline(s);
    equal(s.binding.layout==Layout{},"captured lifecycle requires 33-byte Windows reports with ID 00");
    std::vector<Request> plan{{0x53,{1,0x12,2,0x40,0},1},{0x53,{1,0x12,0},1}};
    for(const auto& r:gripRestore(s.mapping)) {
        equal(r.mode==1&&r.parameters==Bytes{0x40},"lifecycle requires captured mode 01 parameter 40");
        plan.push_back({0x53,{1,r.target,r.mode,0x40},1});
    }
    return plan;
}
Session::Session(Io& io, Trace& trace):io_(io),trace_(trace),binding_(io.binding()) {
    binding_.layout.validate();last_=io.now();
    if(last_.count()<0||last_>Time::max()-Time(75000000))throw ProtocolError("invalid monotonic origin");
    totalDeadline_=last_+Time(75000000);
}
[[noreturn]] void Session::stop(const std::string& reason) {
    if(failure_.empty()) failure_=reason;
    throw ProtocolError(failure_);
}
void Session::guard() {
    if(failed())throw ProtocolError(failure_);
    const auto current=io_.now();
    if(current<last_ || current>=totalDeadline_ || (begun_&&current>=deadline_))stop("monotonic deadline expired");
    last_=current;
    if(!trace_.healthy())stop("trace incomplete");
    if(binding_!=io_.binding())stop("interface binding changed");
    if(scopedGuard_)scopedGuard_();
}
void Session::begin(const std::string& phase, Time duration, unsigned attempts, std::vector<Request> exactPlan) {
    if(begun_&&!ended_)stop("previous phase incomplete");
    guard();if(duration.count()<=0 || duration>Time(30000000) || attempts==0 || attempts>128)stop("invalid phase limits");
    deadline_=std::min(totalDeadline_,io_.now()+duration);budget_=attempts;attempts_=0;index_=0;plan_=std::move(exactPlan);begun_=true;ended_=false;
    try {trace_.record(io_.now(),"phase_"+phase);guard();}catch(const std::exception& e){stop(e.what());}
}
void Session::end() {
    guard();if(!begun_||ended_||(!plan_.empty()&&index_!=plan_.size()))stop("phase incomplete");ended_=true;
}
Bytes Session::exchange(const Request& r) {
    try {
        guard();if(!begun_||ended_||attempts_>=budget_)stop("request budget exhausted or phase inactive");
        const bool exact=!plan_.empty();
        if(exact&&(index_>=plan_.size()||r!=plan_[index_]))stop("request differs from ordered plan");
        const bool authorized=exact&&!authorizedActive_.empty()&&plan_==authorizedActive_;
        validateRequest(r,exact&&(!io_.physical()||authorized));
        if(r.command==0x53&&actuators_==0&&beforeEntry_)beforeEntry_();
        if(!io_.stillSameDevice())stop("selected device removed or replaced");guard();
        // Poll/drain under the same deadline. Every report is traced, including stale ones.
        for(unsigned n=0;;++n) {
            if(n==64)stop("drain report budget exhausted");
            guard();const auto drained=io_.read(deadline_,true);
            if(!drained.bytes.empty())trace_.record(io_.now(),"drain",drained.bytes,operation_);
            guard();
            if(drained.status==Completion::Idle)break;
            if(drained.status!=Completion::Complete||drained.bytes.empty()||n>=64)stop("drain failed or report budget exhausted");
            // Ambiguous stale same-opcode reply is not permission to submit a new request.
            const auto body=binding_.layout.unwrap(drained.bytes);
            if(body.size()>=3&&body[0]==0x5a&&body[1]==0xa5&&body[2]==r.command)stop("stale same-opcode reply; attribution uncertain");
        }
        if(r.command==0x53 && ++modes_>4)stop("mode budget exhausted");
        if(r.command==0x57 && ++waves_>3)stop("waveform budget exhausted");
        const auto wire=binding_.layout.wrap(frame(r.command,r.payload));
        ++attempts_;++index_;++operation_;
        trace_.record(io_.now(),"write_attempt",wire,operation_);guard();
        if(r.command==0x53&&actuators_==0&&beforeEntry_)beforeEntry_();
        if(r.command==0x53)uncertain_=true;
        if(r.command==0x53||r.command==0x57)++actuators_;else ++queries_;
        const auto exchangeDeadline=std::min(deadline_,io_.now()+Time(600000));
        auto written=io_.write(wire,exchangeDeadline);
        trace_.record(io_.now(),"write_return",{},operation_,written.transferred);
        trace_.record(io_.now(),"write_status",{},operation_,(static_cast<std::uint64_t>(written.status)<<32)|written.error);guard();
        if(written.status!=Completion::Complete||written.transferred!=wire.size())stop("write incomplete; no retry");
        if(io_.now()>=exchangeDeadline)stop("write completed after exchange deadline");
        if(r.replySize==0)return {};
        auto received=io_.read(exchangeDeadline,false);
        trace_.record(io_.now(),"receive",received.bytes,operation_,received.error);guard();
        trace_.record(io_.now(),"receive_status",{},operation_,(static_cast<std::uint64_t>(received.status)<<32)|received.transferred);guard();
        if(received.status!=Completion::Complete || received.transferred!=received.bytes.size())stop("reply missing or incomplete");
        if(io_.now()>=exchangeDeadline)stop("late reply; attribution uncertain");
        if(r.command==0x53)observationEligible_=true;
        Bytes body;
        try {body=binding_.layout.unwrap(received.bytes);}
        catch(...) {
            if(lifecyclePolicy_&&r.command==0x53) {
                lastModeReply_=ModeReply::InvalidUnexpected;
                trace_.record(io_.now(),"mode_reply_invalid_unexpected",received.bytes,operation_);
            }
            throw;
        }
        if(lifecyclePolicy_&&r.command==0x53) {
            lastModeReply_=classifyModeReply(body);
            trace_.record(io_.now(),std::string("mode_reply_")+modeReplyName(lastModeReply_),received.bytes,operation_);guard();
            if(!io_.stillSameDevice())stop("lifecycle device lost after reply");guard();
            const bool restore=index_==3||index_==4;
            if(lastModeReply_==ModeReply::NormalSuccessAck)return {0};
            if(restore&&lastModeReply_==ModeReply::CapturedZeroCountValue1)return {};
            stop("lifecycle mode reply rejected");
        }
        std::size_t length=r.replySize;
        if(r.variableChunk) {if(body.size()<11||body[10]>16)stop("invalid RAM chunk size");length=8+body[10];}
        auto p=replyPayload(body,r.command,length,r.v21);
        if(!p)stop("unexpected/unrelated reply");
        trace_.record(io_.now(),"parse_payload",*p,operation_);guard();
        if(r.command==0x53)requireModeSuccess(*p);
        return *p;
    } catch(const std::exception& e) {stop(e.what());}
}
Snapshot acquireSnapshot(Session& t) {
    try {
        Snapshot s;s.binding=t.binding();s.schema=s.binding.access==AccessMode::Unknown?1:2;
        s.info=identity(t.exchange(query(1)));s.unit=uid(t.exchange(query(4)));
        equal(s.info==identity(t.exchange(query(1))),"identity changed during acquisition");
        s.formats=versions(t.exchange(query(7)));s.config=configState(t.exchange(query(0xa1)));
        for(std::size_t n=0;n<4;++n)s.fingerprints[n]=ramInfo(ids[n],t.exchange(ramQuery(ids[n])));
        unsigned requests=25;
        for(auto f:s.fingerprints) {equal(f.slot==s.config.slot&&f.length>0,"RAM slot/length mismatch");requests+=(f.length+15)/16;}
        equal(requests<=128,"snapshot exceeds request budget");
        for(std::size_t n=0;n<4;++n) {
            auto before=ramInfo(ids[n],t.exchange(ramQuery(ids[n])));equal(before==s.fingerprints[n],"RAM changed before read");
            std::vector<Bytes> chunks;for(unsigned i=0;i<(before.length+15u)/16;++i)chunks.push_back(t.exchange(chunk(ids[n],static_cast<std::uint8_t>(i))));
            auto after=ramInfo(ids[n],t.exchange(ramQuery(ids[n])));s.blocks[n]=assembleRam(before,after,chunks);
        }
        for(std::size_t n=0;n<4;++n)equal(s.fingerprints[n]==ramInfo(ids[n],t.exchange(ramQuery(ids[n]))),"RAM changed after acquisition");
        equal(s.config==configState(t.exchange(query(0xa1))),"active configuration changed");
        equal(s.formats==versions(t.exchange(query(7))),"formats changed");equal(s.unit==uid(t.exchange(query(4))),"UID changed");
        equal(s.info==identity(t.exchange(query(1))),"firmware/identity changed during snapshot");
        validateSnapshot(s);t.guard();return s;
    }catch(const std::exception& e){t.stop(e.what());}
}
GripBaseline acquireGripBaseline(Session& t) {
    try {
        GripBaseline s;s.binding=t.binding();s.physicalOrigin=t.physical();
        equal(s.physicalOrigin?s.binding.access==AccessMode::Shared:s.binding.access==AccessMode::Synthetic,"grip acquisition requires explicit shared physical or synthetic access");
        t.begin("grip_readback",Time(30000000),14,gripBaselinePlan());
        s.info=identity(t.exchange(query(1)));s.unit=uid(t.exchange(query(4)));
        s.formats=versions(t.exchange(query(7)));s.config=configState(t.exchange(query(0xa1)));
        s.motor=ramInfo(6,t.exchange(ramQuery(6)));
        equal(s.motor.slot==s.config.slot&&s.motor.length==64,"grip RAM slot/length mismatch");
        std::vector<Bytes> chunks;for(unsigned i=0;i<4;++i)chunks.push_back(t.exchange(chunk(6,static_cast<std::uint8_t>(i))));
        const auto after=ramInfo(6,t.exchange(ramQuery(6)));s.mapping=assembleRam(s.motor,after,chunks);
        equal(s.config==configState(t.exchange(query(0xa1))),"active configuration changed during grip acquisition");
        equal(s.formats==versions(t.exchange(query(7))),"formats changed during grip acquisition");
        equal(s.unit==uid(t.exchange(query(4))),"UID changed during grip acquisition");
        equal(s.info==identity(t.exchange(query(1))),"firmware/identity changed during grip acquisition");
        validateGripBaseline(s);t.end();return s;
    }catch(const std::exception& e){t.stop(e.what());}
}
namespace {
RunResult runGripNeutralLifecycle(Session& t,const GripBaseline& baseline) {
    RunResult result;
    try {
        validateGripBaseline(baseline);
        // A physical baseline can be rehearsed only against an explicitly synthetic
        // copy. Neither the copy nor this result can authorize device writes.
        auto expected=baseline;if(!t.physical()){expected.physicalOrigin=false;expected.binding.access=AccessMode::Synthetic;}
        equal(acquireGripBaseline(t)==expected,"fresh grip baseline differs");
        const auto plan=neutralPlan(baseline);t.begin("grip_neutral_rehearsal",Time(15000000),21,plan);const auto started=t.now();
        Bytes mapping;
        for(std::size_t i=0;i<plan.size();++i) {
            if(i==14&&t.now()-started>=Time(10000000))t.stop("cleanup start deadline missed");
            const auto p=t.exchange(plan[i]);const auto cmd=plan[i].command;
            if(cmd==1)equal(identity(p)==baseline.info,"identity changed");
            if(cmd==4)equal(uid(p)==baseline.unit,"UID changed");
            if(cmd==7)equal(versions(p)==baseline.formats,"formats changed");
            if(cmd==0xa1)equal(configState(p)==baseline.config,"profile changed");
            if(cmd==0xa3) {
                const Bytes header{0,0,6,baseline.config.slot,64,0,static_cast<std::uint8_t>(i-5),16};
                equal(p.size()==24&&std::equal(header.begin(),header.end(),p.begin()),"motor map chunk mismatch");
                mapping.insert(mapping.end(),p.begin()+8,p.end());if(i==8)equal(mapping==baseline.mapping,"motor map changed");
            }
        }
        t.end();equal(acquireGripBaseline(t)==expected,"postflight grip baseline changed");result.complete=true;
    }catch(const std::exception& e){if(!t.failed()){try{t.stop(e.what());}catch(...){}}result.failure=t.failure();}
    result.deviceStateUncertain=t.uncertain();return result;
}
}
RunResult rehearseNeutral(Session& t,const GripBaseline& baseline) {
    if(t.physical())return {false,false,"physical grip neutral execution locked; rehearsal only"};
    return runGripNeutralLifecycle(t,baseline);
}
void Session::observeRestoreTail() {
    // Only the distinct restore-observation lifecycle calls this after a full
    // write and a timely, complete first reply. Normal guard() remains latched:
    // no subsequent exchange/begin/write may resume, even after a late ACK.
    observation_.firstReply=failure_.empty()?"normal_mode_ack":failure_;
    if(failure_.empty())failure_="restore observation only; physical recovery not established";
    if(!observationEligible_||!trace_.healthy())return;
    const auto start=io_.now();auto previous=start;
    const auto deadline=std::min({start+Time(500000),deadline_,totalDeadline_});
    observation_.started=true;
    auto check=[&] {
        const auto current=io_.now();
        if(current<previous)throw ProtocolError("observation clock moved backwards");
        previous=current;
        if(!trace_.healthy())throw ProtocolError("observation trace incomplete");
        if(binding_!=io_.binding()||!io_.stillSameDevice())throw ProtocolError("observation device removed or replaced");
        return current;
    };
    try {
        trace_.record(start,"restore_observation_begin",{},operation_,500000);
        while(observation_.reports<32) {
            if(check()>=deadline){observation_.stop="deadline";observation_.complete=true;break;}
            const auto received=io_.read(deadline,false);
            trace_.record(io_.now(),"restore_observation_receive",received.bytes,operation_,received.error);
            trace_.record(io_.now(),"restore_observation_status",{},operation_,(static_cast<std::uint64_t>(received.status)<<32)|received.transferred);
            const auto current=check();
            if(received.status==Completion::Timeout&&received.bytes.empty()) {
                if(current<deadline)throw ProtocolError("observation timeout before deadline");
                observation_.stop="deadline";observation_.complete=true;break;
            }
            if(received.status!=Completion::Complete||received.transferred!=received.bytes.size()||received.bytes.size()!=binding_.layout.inputLength)
                throw ProtocolError("observation I/O incomplete");
            ++observation_.reports;
            if(current>deadline)throw ProtocolError("observation completion after deadline");
            // Raw evidence only. No parser result can authorize another write.
        }
        if(observation_.reports==32){observation_.stop="report_limit";observation_.complete=true;}
        trace_.record(io_.now(),"restore_observation_end",{},operation_,observation_.reports);
        if(!trace_.healthy())throw ProtocolError("observation trace incomplete");
    }catch(const std::exception& e){observation_.complete=false;observation_.stop="fault";observation_.error=e.what();}
    observation_.elapsed=io_.now()-start;
}
RunResult runGripRestoreLifecycle(Session& t,const GripBaseline& baseline,bool observe) {
    RunResult result;
    try {
        validateGripBaseline(baseline);auto expected=baseline;
        if(!t.physical()){expected.physicalOrigin=false;expected.binding.access=AccessMode::Synthetic;}
        equal(acquireGripBaseline(t)==expected,"fresh restore-only baseline differs");
        const auto plan=gripRestoreLeftPlan(baseline);
        t.begin(observe?"grip_restore_observe":"grip_restore_left_only",Time(2000000),1,plan);
        if(observe) {
            try{t.exchange(plan.front());}catch(const std::exception& e){if(!t.failed())t.failure_=e.what();}
            t.observeRestoreTail();
            return {false,t.uncertain(),t.failure()};
        }
        t.exchange(plan.front());t.end();
        equal(acquireGripBaseline(t)==expected,"restore-only postflight changed");result.complete=true;
    }catch(const std::exception& e){if(!t.failed()){try{t.stop(e.what());}catch(...){}}result.failure=t.failure();}
    result.deviceStateUncertain=t.uncertain();return result;
}
RunResult rehearseGripRestore(Session& t,const GripBaseline& baseline,bool observe) {
    if(t.physical())return {false,false,"physical restore-only execution requires separate approval"};
    return runGripRestoreLifecycle(t,baseline,observe);
}
GripLifecycleResult runGripLifecycle(Session& t,const GripBaseline& baseline) {
    GripLifecycleResult result;
    try {
        const auto plan=gripLifecyclePlan(baseline);auto expected=baseline;
        if(!t.physical()){expected.physicalOrigin=false;expected.binding.access=AccessMode::Synthetic;}
        equal(acquireGripBaseline(t)==expected,"fresh lifecycle baseline differs");
        t.begin("grip_mode_only_lifecycle",Time(5000000),4,plan);t.lifecyclePolicy_=true;
        for(std::size_t i=0;i<plan.size();++i) {
            t.lastModeReply_=ModeReply::NotObserved;
            try {t.exchange(plan[i]);}catch(...) {if(i>=2)result.restoreReplies[i-2]=t.lastModeReply_;throw;}
            if(i>=2)result.restoreReplies[i-2]=t.lastModeReply_;
        }
        t.end();t.lifecyclePolicy_=false;result.sequenceComplete=true;
        equal(acquireGripBaseline(t)==expected,"lifecycle postflight changed");
        if(!t.io_.stillSameDevice())t.stop("lifecycle device lost after postflight");t.guard();result.postflightMatches=true;
    }catch(const std::exception& e){if(!t.failed()){try{t.stop(e.what());}catch(...){}}result.failure=t.failure();}
    t.lifecyclePolicy_=false;result.deviceStateUncertain=t.uncertain();return result;
}
GripLifecycleResult rehearseGripLifecycle(Session& t,const GripBaseline& baseline) {
    if(t.physical()){GripLifecycleResult r;r.failure="physical lifecycle requires separate approval";return r;}
    return runGripLifecycle(t,baseline);
}
std::string encodeGripLifecycleApproval(const GripLifecycleApproval& a) {
    return std::string("ASB_APEX6_GRIP_LIFECYCLE_APPROVAL_V1\n")+gripLifecycleScope+"\n"+encodeGripRestoreApproval(a.checkpoints);
}
GripLifecycleApproval decodeGripLifecycleApproval(const std::string& text) {
    const auto prefix=std::string("ASB_APEX6_GRIP_LIFECYCLE_APPROVAL_V1\n")+gripLifecycleScope+"\n";
    equal(text.starts_with(prefix),"lifecycle approval scope mismatch");
    GripLifecycleApproval a{decodeGripRestoreApproval(text.substr(prefix.size()))};
    equal(!a.checkpoints.observeRepliesAccepted&&encodeGripLifecycleApproval(a)==text,"invalid lifecycle approval");return a;
}
GripLifecycleAuthorization GripLifecycleAuthorization::approve(const GripBaseline& baseline,const GripLifecycleApproval& approval,const std::string& hash,std::int64_t now) {
    gripLifecyclePlan(baseline);GripLifecycleAuthorization a;a.baseline_=baseline;a.approval_=approval;a.hash_=hash;a.check(now);return a;
}
void GripLifecycleAuthorization::check(std::int64_t now)const {
    // Reuse checkpoint validation, never the old token decoder or authorization.
    GripRestoreAuthorization::approve(baseline_,approval_.checkpoints,hash_,now,false);
}
void GripLifecycleAuthorization::consume()const {
    if(consumed_->exchange(true))throw ProtocolError("lifecycle authorization already consumed");
}
GripLifecycleResult runAuthorizedGripLifecycle(Session& t,const GripLifecycleAuthorization& a,const std::function<std::int64_t()>& clock) {
    GripLifecycleResult result;
    try {
        a.check(clock());
        if(!t.physical()||t.binding()!=a.baseline().binding)t.stop("authorized lifecycle binding mismatch");
        t.authorizedActive_=gripLifecyclePlan(a.baseline());t.beforeEntry_=[&]{a.check(clock());};
        result=runGripLifecycle(t,a.baseline());
    }catch(const std::exception& e){if(!t.failed()){try{t.stop(e.what());}catch(...){}}result.failure=t.failure();result.deviceStateUncertain=t.uncertain();}
    t.beforeEntry_={};t.authorizedActive_.clear();return result;
}
RunResult runNeutralLifecycle(Session& t,const Snapshot& baseline) {
    RunResult result;
    try {
        const auto snap=snapshotPlan(baseline);
        t.begin("preflight",Time(30000000),128,snap);equal(acquireSnapshot(t)==baseline,"fresh snapshot differs from baseline");t.end();
        const auto active=neutralPlan(baseline);t.begin("active",Time(15000000),32,active);const auto started=t.now();
        Bytes map;
        for(std::size_t i=0;i<active.size();++i) {
            if(i==14 && t.now()-started>=Time(10000000))t.stop("cleanup start deadline missed");
            auto p=t.exchange(active[i]);auto cmd=active[i].command;
            if(cmd==1)equal(identity(p)==baseline.info,"identity changed");
            if(cmd==4)equal(uid(p)==baseline.unit,"UID changed");
            if(cmd==7)equal(versions(p)==baseline.formats,"formats changed");
            if(cmd==0xa1)equal(configState(p)==baseline.config,"profile changed");
            if(cmd==0xa3) {
                const Bytes header{0,0,6,baseline.config.slot,64,0,static_cast<std::uint8_t>(i-5),16};
                equal(p.size()==24&&std::equal(header.begin(),header.end(),p.begin()),"motor map chunk mismatch");
                map.insert(map.end(),p.begin()+8,p.end());
                if(i==8)equal(map==baseline.blocks[3],"motor map changed");
            }
        }
        t.end();t.begin("postflight",Time(30000000),128,snap);equal(acquireSnapshot(t)==baseline,"postflight configuration changed");t.end();result.complete=true;
    } catch(const std::exception& e) {if(!t.failed()) {try{t.stop(e.what());}catch(...) {}}result.failure=t.failure();}
    // Even successful software rehearsal cannot establish physical recovery.
    result.deviceStateUncertain=t.uncertain();return result;
}
RunResult rehearseNeutral(Session& t,const Snapshot& baseline) {
    if(t.physical())return {false,false,"physical neutral execution locked; rehearsal only"};
    return runNeutralLifecycle(t,baseline);
}
bool validApproval(const Approval& a,const std::string& hash,std::int64_t now) {
    return hash.size()==64&&std::all_of(hash.begin(),hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');})&&a.manifestHash==hash&&a.approved&&a.confirmedUnixSeconds>=0&&now>=a.confirmedUnixSeconds&&now-a.confirmedUnixSeconds<=300&&
        a.observer&&a.powerOffReady&&a.normalVibration&&a.knownWritersQuiesced&&a.directUsb&&a.allDisabledAccepted&&a.failStopAccepted&&a.backgroundRiskAccepted&&a.restorationRiskAccepted;
}
std::string encodeApproval(const Approval& a) {
    std::ostringstream out;out<<"ASB_APEX6_APPROVAL_V2\n"<<a.manifestHash<<'\n'<<a.confirmedUnixSeconds<<'\n'
        <<a.approved<<' '<<a.observer<<' '<<a.powerOffReady<<' '<<a.normalVibration<<' '<<a.knownWritersQuiesced<<' '<<a.directUsb<<' '<<a.allDisabledAccepted<<' '<<a.failStopAccepted<<' '<<a.backgroundRiskAccepted<<' '<<a.restorationRiskAccepted<<'\n';return out.str();
}
Approval decodeApproval(const std::string& text) {
    if(text.size()>1024)throw ProtocolError("approval too large");std::istringstream in(text);std::string schema;Approval a;
    if(!(in>>schema>>a.manifestHash>>a.confirmedUnixSeconds>>a.approved>>a.observer>>a.powerOffReady>>a.normalVibration>>a.knownWritersQuiesced>>a.directUsb>>a.allDisabledAccepted>>a.failStopAccepted>>a.backgroundRiskAccepted>>a.restorationRiskAccepted)||schema!="ASB_APEX6_APPROVAL_V2"||encodeApproval(a)!=text)throw ProtocolError("invalid/noncanonical approval");return a;
}
NeutralAuthorization NeutralAuthorization::approve(const Snapshot& baseline,const Approval& approval,const std::string& hash,std::int64_t now) {
    validateSnapshot(baseline);
    if(baseline.schema!=2||(baseline.binding.access!=AccessMode::Exclusive&&baseline.binding.access!=AccessMode::Shared))throw ProtocolError("neutral execution requires fresh physical v2 baseline");
    NeutralAuthorization result;result.baseline_=baseline;result.approval_=approval;result.hash_=hash;result.check(now);return result;
}
void NeutralAuthorization::check(std::int64_t now)const {if(!validApproval(approval_,hash_,now))throw ProtocolError("neutral approval missing, expired or mismatched");}
std::string encodeGripApproval(const GripApproval& a) {
    return "ASB_APEX6_GRIP_APPROVAL_V1\ngrip-only\n"+std::string(a.reducedAuditAccepted?"1\n":"0\n")+encodeApproval(a.checkpoints);
}
GripApproval decodeGripApproval(const std::string& text) {
    const std::string prefix="ASB_APEX6_GRIP_APPROVAL_V1\ngrip-only\n";
    if(text.size()>1200||!text.starts_with(prefix)||text.size()<prefix.size()+2||text[prefix.size()+1]!='\n'||(text[prefix.size()]!='0'&&text[prefix.size()]!='1'))throw ProtocolError("invalid grip approval schema/scope");
    GripApproval a{decodeApproval(text.substr(prefix.size()+2)),text[prefix.size()]=='1'};
    if(encodeGripApproval(a)!=text)throw ProtocolError("noncanonical grip approval");return a;
}
GripNeutralAuthorization GripNeutralAuthorization::approve(const GripBaseline& baseline,const GripApproval& approval,const std::string& hash,std::int64_t now) {
    validateGripBaseline(baseline);
    if(!baseline.physicalOrigin||baseline.binding.access!=AccessMode::Shared)throw ProtocolError("grip neutral requires a physical shared baseline");
    GripNeutralAuthorization a;a.baseline_=baseline;a.approval_=approval;a.hash_=hash;a.check(now);return a;
}
void GripNeutralAuthorization::check(std::int64_t now) const {
    if(!approval_.reducedAuditAccepted||!validApproval(approval_.checkpoints,hash_,now))throw ProtocolError("grip approval missing, expired, mismatched or reduced audit not accepted");
}
RunResult runAuthorizedGripNeutral(Session& t,const GripNeutralAuthorization& authorization,const std::function<std::int64_t()>& clock) {
    try {
        authorization.check(clock());
        if(!t.physical()||t.binding()!=authorization.baseline().binding)t.stop("authorized grip device/access binding mismatch");
        t.authorizedActive_=neutralPlan(authorization.baseline());t.beforeEntry_=[&]{authorization.check(clock());};
        auto result=runGripNeutralLifecycle(t,authorization.baseline());t.beforeEntry_={};t.authorizedActive_.clear();return result;
    }catch(const std::exception& e){t.beforeEntry_={};t.authorizedActive_.clear();if(!t.failed()){try{t.stop(e.what());}catch(...){}}return {false,t.uncertain(),t.failure()};}
}
std::string encodeGripRestoreApproval(const GripRestoreApproval& a) {
    std::ostringstream out;out<<(a.observeRepliesAccepted?"ASB_APEX6_GRIP_OBSERVE_APPROVAL_V1\ngrip-left-restore-observe\n":"ASB_APEX6_GRIP_RESTORE_APPROVAL_V1\ngrip-left-restore-only\n")<<a.manifestHash<<'\n'<<a.confirmedUnixSeconds<<'\n'
        <<a.approved<<' '<<a.observer<<' '<<a.powerOffReady<<' '<<a.normalVibration<<' '<<a.knownWritersQuiesced<<' '<<a.directUsb<<' '
        <<a.failStopAccepted<<' '<<a.backgroundRiskAccepted<<' '<<a.restorationRiskAccepted<<' '<<a.reducedAuditAccepted<<' '<<a.powerCycled<<' '<<a.restoreOnlyAccepted<<'\n';return out.str();
}
GripRestoreApproval decodeGripRestoreApproval(const std::string& text) {
    if(text.size()>1200)throw ProtocolError("restore approval too large");std::istringstream in(text);std::string schema,scope;GripRestoreApproval a;
    if(!(in>>schema>>scope>>a.manifestHash>>a.confirmedUnixSeconds>>a.approved>>a.observer>>a.powerOffReady>>a.normalVibration>>a.knownWritersQuiesced>>a.directUsb
         >>a.failStopAccepted>>a.backgroundRiskAccepted>>a.restorationRiskAccepted>>a.reducedAuditAccepted>>a.powerCycled>>a.restoreOnlyAccepted)
       )throw ProtocolError("invalid restore approval");
    a.observeRepliesAccepted=schema=="ASB_APEX6_GRIP_OBSERVE_APPROVAL_V1"&&scope=="grip-left-restore-observe";
    if(encodeGripRestoreApproval(a)!=text)throw ProtocolError("invalid/noncanonical restore approval");return a;
}
GripRestoreAuthorization GripRestoreAuthorization::approve(const GripBaseline& baseline,const GripRestoreApproval& approval,const std::string& hash,std::int64_t now,bool observe) {
    validateGripBaseline(baseline);
    if(approval.observeRepliesAccepted!=observe)throw ProtocolError("restore observation approval scope mismatch");
    if(!baseline.physicalOrigin||baseline.binding.access!=AccessMode::Shared)throw ProtocolError("restore-only requires physical shared baseline");
    GripRestoreAuthorization result;result.baseline_=baseline;result.approval_=approval;result.hash_=hash;result.check(now);return result;
}
void GripRestoreAuthorization::check(std::int64_t now) const {
    const auto& a=approval_;
    if(hash_.size()!=64||!std::all_of(hash_.begin(),hash_.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');})||a.manifestHash!=hash_
       ||a.confirmedUnixSeconds<0||now<a.confirmedUnixSeconds||now-a.confirmedUnixSeconds>300||!a.approved||!a.observer||!a.powerOffReady||!a.normalVibration
       ||!a.knownWritersQuiesced||!a.directUsb||!a.failStopAccepted||!a.backgroundRiskAccepted||!a.restorationRiskAccepted||!a.reducedAuditAccepted||!a.powerCycled||!a.restoreOnlyAccepted)
        throw ProtocolError("restore-only approval missing, expired, mismatched or incomplete");
}
RunResult runAuthorizedGripRestore(Session& t,const GripRestoreAuthorization& authorization,const std::function<std::int64_t()>& clock) {
    try {
        authorization.check(clock());
        if(!t.physical()||t.binding()!=authorization.baseline().binding)t.stop("authorized restore-only device/access binding mismatch");
        t.authorizedActive_=gripRestoreLeftPlan(authorization.baseline());t.beforeEntry_=[&]{authorization.check(clock());};
        const auto result=runGripRestoreLifecycle(t,authorization.baseline(),authorization.observeReplies());t.beforeEntry_={};t.authorizedActive_.clear();return result;
    }catch(const std::exception& e){t.beforeEntry_={};t.authorizedActive_.clear();if(!t.failed()){try{t.stop(e.what());}catch(...){}}return {false,t.uncertain(),t.failure()};}
}
RunResult runAuthorizedNeutral(Session& t,const NeutralAuthorization& authorization,const std::function<std::int64_t()>& clock) {
    try {
        authorization.check(clock());
        if(t.binding()!=authorization.baseline().binding)t.stop("authorized device/access binding mismatch");
        t.authorizedActive_=neutralPlan(authorization.baseline());
        t.beforeEntry_=[&]{authorization.check(clock());};
        auto result=runNeutralLifecycle(t,authorization.baseline());t.beforeEntry_={};t.authorizedActive_.clear();return result;
    }catch(const std::exception& e){t.beforeEntry_={};t.authorizedActive_.clear();if(!t.failed()){try{t.stop(e.what());}catch(...){}}return {false,t.uncertain(),t.failure()};}
}
}
