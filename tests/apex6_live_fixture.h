#pragma once
#include "apex6/live/Stream.h"
#include <cmath>
// Eight milliseconds of 48-kHz, four-channel PCM, with optional URB padding.
inline asb::apex6::Bytes livePcm(std::uint64_t sequence,std::uint64_t generation=1,unsigned channel=2,std::size_t padding=0) {
    using namespace asb::apex6;Bytes b(96+384*8+padding);
    auto put=[&](unsigned p,std::uint64_t n,unsigned size=4){for(unsigned i=0;i<size;++i)b[p+i]=static_cast<std::uint8_t>(n>>(i*8));};
    b[0]='A';b[1]='S';b[2]='B';b[3]='R';put(4,1,2);put(6,1,2);put(8,b.size());put(12,80);put(16,sequence,8);put(24,generation,8);put(32,sequence*8000000,8);
    put(40,1);put(48,48000);put(52,4,2);put(54,16,2);put(56,1);put(60,384*8+padding);put(68,1);put(72,1);put(84,384*8);put(88,384*8);
    for(unsigned i=0;i<384;++i){const auto sample=static_cast<std::int16_t>(12000*std::sin(i*2*3.141592653589793*125/48000));put(96+i*8+channel*2,static_cast<std::uint16_t>(sample),2);}
    return b;
}

inline asb::apex6::Bytes liveHid(std::uint64_t sequence,unsigned left=255,unsigned right=0,bool control=false,unsigned flags=3,unsigned flags3=0,std::uint64_t generation=1){
    using namespace asb::apex6;Bytes b(80+(control?8:0)+48);
    auto put=[&](unsigned p,std::uint64_t n,unsigned size=4){for(unsigned i=0;i<size;++i)b[p+i]=static_cast<std::uint8_t>(n>>(i*8));};
    b[0]='A';b[1]='S';b[2]='B';b[3]='R';put(4,1,2);put(6,2,2);put(8,b.size());put(12,80);put(16,sequence,8);put(24,generation,8);put(32,sequence*8000000,8);
    put(40,control?0:3);put(60,b.size()-80);put(68,control?3:2);put(72,1);
    const unsigned p=control?88:80;
    if(control){b[80]=0x21;b[81]=9;put(82,0x0202,2);put(84,3,2);put(86,48,2);}
    b[p]=2;b[p+1]=flags;b[p+3]=right;b[p+4]=left;b[p+39]=flags3;return b;
}
