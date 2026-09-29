#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "apex6/experiment/Pulse.h"

namespace asb::apex6::experiment {
// Per-object high-resolution timer, no global timer-resolution change or fallback.
// https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw
class WindowsPulseWait {
public:
    WindowsPulseWait(HANDLE cancelled,std::function<Time()> clock):cancel_(cancelled),clock_(std::move(clock)) {
        timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(!timer_)throw ProtocolError("pulse high-resolution timer unavailable; no fallback");
    }
    ~WindowsPulseWait(){if(timer_)CloseHandle(timer_);}
    WindowsPulseWait(const WindowsPulseWait&)=delete;
    WindowsPulseWait& operator=(const WindowsPulseWait&)=delete;
    bool cancelled()const {
        const auto r=WaitForSingleObject(cancel_,0);
        if(r!=WAIT_TIMEOUT&&r!=WAIT_OBJECT_0)throw ProtocolError("pulse cancellation event failed");
        return r==WAIT_OBJECT_0;
    }
    void waitUntil(Time due) {
        if(cancelled())return;
        auto previous=clock_();const auto remaining=due-previous;if(remaining<=Time{})return;
        if(remaining>Time(10000))throw ProtocolError("pulse wait exceeds bound");
        // Wake before the deadline so timer wake latency does not accumulate at
        // every packet. The last <=1 ms is bounded polling with cancellation.
        // This changes waiting, never the permitted submission time or spacing.
        unsigned wakes=0,spins=0;
        for(;;) {
            if(cancelled())return;
            const auto current=clock_();if(current<previous)throw ProtocolError("pulse wait clock reversed");previous=current;
            if(current>=due)return;
            const auto left=due-current;
            if(left>Time(1000)) {
                if(++wakes>8)throw ProtocolError("pulse timer early-wake limit");
                LARGE_INTEGER relative;relative.QuadPart=-(left.count()-1000)*10;
                if(!SetWaitableTimer(timer_,&relative,0,nullptr,nullptr,FALSE))throw ProtocolError("pulse timer arm failed");
                HANDLE handles[]{cancel_,timer_};const auto r=WaitForMultipleObjects(2,handles,FALSE,15);
                if(r!=WAIT_OBJECT_0&&r!=WAIT_OBJECT_0+1)throw ProtocolError("pulse bounded wait failed");
            } else {
                if(++spins>100000)throw ProtocolError("pulse spin clock stalled");
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
        }
    }
private:
    HANDLE timer_=nullptr,cancel_;std::function<Time()> clock_;
};
}
