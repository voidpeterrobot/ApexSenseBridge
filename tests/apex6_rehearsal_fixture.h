#pragma once
#include "apex6/experiment/Session.h"
#include "fixtures/apex6/ReferenceVectors.h"
// Synthetic RAM and a deliberately synthetic, non-openable identity.
inline asb::apex6::experiment::Snapshot rehearsalFixture() {
    using namespace asb::apex6::experiment;
    Snapshot s;
    s.binding={"SYNTHETIC_ONLY","SYNTHETIC_ONLY","SYNTHETIC_ONLY","synthetic-caps",{}};
    s.binding.access=AccessMode::Synthetic;
    s.info={150,0,0x80,{1,2,3,4,5,6,7}};
    for(unsigned i=0;i<16;++i)s.unit[i]=static_cast<std::uint8_t>(i+1);
    s.formats={1,2,3};s.config={0,{101,102,103,104,105}};
    for(std::size_t i=0;i<4;++i){const auto& r=vectors::ram[i];s.fingerprints[i]={static_cast<std::uint8_t>(r.id),0,static_cast<std::uint16_t>(r.length),r.crc};s.blocks[i]=unhex(r.hex);}
    return s;
}
inline asb::apex6::experiment::GripBaseline gripRehearsalFixture() {
    const auto s=rehearsalFixture();
    return {1,false,s.binding,s.info,s.unit,s.formats,s.config,s.fingerprints[3],s.blocks[3]};
}
