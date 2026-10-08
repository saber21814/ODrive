#pragma once
#include "frame.hpp"

namespace rehab { namespace wire {
// Only the application task produces/consumes these complete commands. Stream
// callbacks retain their separate SPSC byte rings; no ISR waits on this queue.
struct PendingCommand {
    Frame frame{};
    uint32_t received_us=0, epoch=0, fault_generation=0, transport_generation=0;
    unsigned port=0;
};
class CommandQueue {
public:
    static constexpr unsigned capacity=8, dispatch_budget=4;
    static constexpr uint32_t max_age_us=20000;
    bool push(const PendingCommand& command) {
        if(size_==capacity)return false;
        entries_[(head_+size_)%capacity]=command;
        if(++size_>highwater_)highwater_=size_;
        return true;
    }
    bool pop(PendingCommand& command) {
        if(!size_)return false;
        command=entries_[head_];head_=(head_+1)%capacity;--size_;return true;
    }
    unsigned size() const {return size_;}
    unsigned highwater() const {return highwater_;}
private:
    PendingCommand entries_[capacity]{};
    unsigned head_=0,size_=0,highwater_=0;
};
}}
