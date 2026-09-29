#include "apex6/experiment/FakeIo.h"
#include <algorithm>

namespace asb::apex6::experiment {
namespace {
void le(Bytes& b,std::uint32_t value,unsigned count){for(unsigned i=0;i<count;++i)b.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
}
IoResult FakeIo::read(Time deadline,bool poll) {
    ++reads;
    if(readHook)if(auto result=readHook(*this,deadline,poll))return *result;
    if(flood&&poll)return {Completion::Complete,Bytes(snapshot.binding.layout.inputLength,0),snapshot.binding.layout.inputLength};
    if(incoming.empty()) {if(poll)return {Completion::Idle,{}};clock=deadline;return {Completion::Timeout,{}};}
    clock+=readDelay;
    auto bytes=std::move(incoming.front());incoming.pop_front();auto n=bytes.size();return {Completion::Complete,std::move(bytes),n};
}
IoResult FakeIo::write(std::span<const std::uint8_t> wire,Time) {
    const auto submitted=clock;auto result=respond(wire);result.submittedAt=submitted;result.completedAt=clock;return result;
}
IoResult FakeIo::respond(std::span<const std::uint8_t> wire) {
    ++writes;submissions.emplace_back(wire.begin(),wire.end());clock+=writeDelay;
    if(failAt==writes)return {failStatus,{},0,123};
    if(shortWrite)return {Completion::Complete,{},wire.size()-1};
    if(wire.size()<33)throw ProtocolError("fake received short frame");
    const auto cmd=wire[3];Bytes p;
    switch(cmd) {
    case 1:p.resize(25);p[0]=snapshot.info.deviceType;p[1]=snapshot.info.connection;p[24]=snapshot.info.features;
        for(unsigned i=0;i<7;++i){p[10+2*i]=static_cast<std::uint8_t>(snapshot.info.firmware[i]>>8);p[11+2*i]=static_cast<std::uint8_t>(snapshot.info.firmware[i]);}break;
    case 4:p.assign(snapshot.unit.begin(),snapshot.unit.end());break;
    case 7:for(auto v:snapshot.formats)le(p,v,2);break;
    case 0xa1:p.push_back(snapshot.config.slot);for(auto v:snapshot.config.crc16)le(p,v,2);break;
    case 0xa3: {
        const auto id=wire[6];auto found=std::find_if(snapshot.fingerprints.begin(),snapshot.fingerprints.end(),[&](auto f){return f.id==id;});
        if(found==snapshot.fingerprints.end())throw ProtocolError("fake unknown RAM ID");
        const auto index=static_cast<std::size_t>(found-snapshot.fingerprints.begin());const auto& f=*found;
        if(wire[5]==1){p={1,0,id,f.slot};le(p,f.length,2);le(p,f.crc32,4);}
        else {const std::size_t offset=wire[7]*16u;if(offset>=snapshot.blocks[index].size())throw ProtocolError("fake chunk out of range");auto count=std::min<std::size_t>(16,snapshot.blocks[index].size()-offset);
            p={0,0,id,f.slot};le(p,f.length,2);p.push_back(wire[7]);p.push_back(static_cast<std::uint8_t>(count));p.insert(p.end(),snapshot.blocks[index].begin()+offset,snapshot.blocks[index].begin()+offset+count);}
        break;
    }
    case 0x53:p={0};break;
    case 0x57:return {Completion::Complete,{},wire.size()};
    default:throw ProtocolError("fake unknown command");
    }
    if(responseHook)responseHook(*this,cmd,p);
    Bytes report(snapshot.binding.layout.inputLength);report[0]=snapshot.binding.layout.inputId;report[1]=0x5a;report[2]=0xa5;report[3]=cmd;
    const std::size_t start=cmd==0xa3?4:6;
    if(cmd!=0xa3)report[4]=1;
    std::copy(p.begin(),p.end(),report.begin()+start);
    for(std::size_t i=3;i<report.size()-1;++i)report.back()+=report[i];
    if(rawResponseHook)rawResponseHook(*this,report);
    incoming.push_back(report);return {Completion::Complete,{},wire.size()};
}
}
