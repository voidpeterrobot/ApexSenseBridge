#pragma once
#include "apex6/Gpa6.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace asb::apex6::dongle {
enum class PulseSide { None, Left, Right, Both };
inline bool validPulseSide(PulseSide side){return side==PulseSide::Left||side==PulseSide::Right||side==PulseSide::Both;}
inline const char* pulseName(PulseSide side){
    switch(side){case PulseSide::Left:return "left";case PulseSide::Right:return "right";case PulseSide::Both:return "both";default:throw ProtocolError("invalid dongle pulse side");}
}
inline const char* pulseScope(PulseSide side){
    switch(side){case PulseSide::Left:return "apex6-dongle-left-pulse-v1";case PulseSide::Right:return "apex6-dongle-right-pulse-v1";case PulseSide::Both:return "apex6-dongle-both-pulse-v1";default:throw ProtocolError("invalid dongle pulse side");}
}
inline constexpr unsigned pulsePackets=32, pulseMaximumWaves=pulsePackets+3;
// Fixed 256-ms pulse at 1/16 full-scale: left 80 Hz, right 160 Hz.
// No live gain/input. Side is fixed before native transport promotion.
inline Frame pulsePacket(unsigned index,PulseSide side=PulseSide::Left) {
    if(index>=pulsePackets||!validPulseSide(side))throw ProtocolError("dongle pulse packet index/side");
    std::array<std::array<double,2>,8> samples{};
    for(unsigned sample=0;sample<8;++sample){
        if(side!=PulseSide::Right)samples[sample][0]=.0625*std::sin(2*std::numbers::pi*80*(index*8+sample)/1000.0);
        if(side!=PulseSide::Left)samples[sample][1]=.0625*std::sin(2*std::numbers::pi*160*(index*8+sample)/1000.0);
    }
    return gripWaveform(samples);
}
inline bool allowedPulseWave(std::span<const std::uint8_t> wire,unsigned waveIndex,PulseSide side=PulseSide::Left) {
    if(!validPulseSide(side)||wire.size()!=33||wire[0]!=0||waveIndex>=pulseMaximumWaves)return false;
    const auto matches=[&](const Frame& body){return std::equal(body.begin(),body.end(),wire.begin()+1);};
    // Neutral is permitted for the lead, tail and an early orderly stop. The
    // independent lifecycle guard still requires the lead/tail to be neutral.
    return matches(gripWaveform({}))||(waveIndex>=1&&waveIndex<=pulsePackets&&matches(pulsePacket(waveIndex-1,side)));
}
}
