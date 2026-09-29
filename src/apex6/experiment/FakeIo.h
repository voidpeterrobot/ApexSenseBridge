#pragma once
#include "apex6/experiment/Session.h"
#include <deque>

namespace asb::apex6::experiment {
// Explicit synthetic peer; never discovers or opens a device.
class FakeIo final:public Io {
public:
    explicit FakeIo(Snapshot s):snapshot(std::move(s)){}
    explicit FakeIo(const GripBaseline& s) {
        validateGripBaseline(s);
        // Internal reply storage only: absent unrelated blocks are never exported
        // as a full Snapshot. Rehearsals always retain synthetic provenance.
        snapshot.binding=s.binding;snapshot.binding.access=AccessMode::Synthetic;
        snapshot.info=s.info;snapshot.unit=s.unit;snapshot.formats=s.formats;snapshot.config=s.config;
        snapshot.fingerprints[3]=s.motor;snapshot.blocks[3]=s.mapping;
    }
    Time now()const override{return clock;}
    bool physical()const override{return pretendPhysical;}
    const Binding& binding()const override{return snapshot.binding;}
    bool stillSameDevice()override{return present;}
    IoResult read(Time deadline,bool poll)override;
    IoResult write(std::span<const std::uint8_t>,Time deadline)override;
    Snapshot snapshot;
    Time clock{},writeDelay{},readDelay{};
    bool pretendPhysical=false,present=true,flood=false,shortWrite=false;
    unsigned writes=0,reads=0,failAt=0;
    Completion failStatus=Completion::Failed;
    std::function<void(FakeIo&,std::uint8_t,Bytes&)> responseHook;
    std::function<void(FakeIo&,Bytes&)> rawResponseHook; // captured-envelope fault fixtures
    std::function<std::optional<IoResult>(FakeIo&,Time,bool)> readHook;
    std::deque<Bytes> incoming;
    std::vector<Bytes> submissions;
private:
    IoResult respond(std::span<const std::uint8_t>);
};
}
