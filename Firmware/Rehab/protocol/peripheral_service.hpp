#pragma once
#include "frame.hpp"

namespace rehab { namespace wire {
// Preserve the existing wire result values without depending on the training
// engine. Unsupported commands keep their old numbers and never gain effects.
enum class PeripheralResult : uint16_t {
    Ok=0, InvalidState, InvalidConfig, Busy, NotOwner, Faulted,
    Unsupported, Stale, BadSequence, QueueFull, Inhibited
};
struct PeripheralHooks {
    void* ctx=nullptr;
    uint32_t (*new_session)(void*)=nullptr;
    uint32_t (*diagnostic)(void*,unsigned)=nullptr;
    PeripheralResult (*network)(void*,const uint8_t*,size_t)=nullptr;
    void (*stop)(void*)=nullptr;
};

// Owned by the peripheral service task. Transport callbacks only enqueue bytes;
// they must not call handle()/disconnect() concurrently.
class PeripheralService {
public:
    explicit PeripheralService(const PeripheralHooks& hooks):hooks_(hooks) {}
    void handle(unsigned transport,const Frame& request,Frame& response);
    // Admission failure must still replay completed requests, and must not
    // advance the watermark beyond older requests waiting in the task queue.
    void reject_admission(unsigned transport,const Frame& request,Frame& response,
                          PeripheralResult result);
    void disconnect(unsigned transport);
private:
    struct Connection { uint32_t session=0; Replay replay{}; };
    PeripheralResult execute(const Frame&,Frame&);
    PeripheralHooks hooks_;
    Connection connections_[3]{};
};
}}
