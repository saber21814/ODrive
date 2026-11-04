#pragma once
#include "frame.hpp"
#include "config_fields.hpp"

namespace rehab { namespace wire {
constexpr unsigned kDiagnosticWords=63;
struct Hooks {
    void* ctx=nullptr;
    uint32_t (*new_session)(void*)=nullptr;
    Result (*acquire)(void*,uint32_t,const Feedback&)=nullptr;
    Result (*release)(void*,uint32_t,const Feedback&)=nullptr;
    bool (*motion_allowed)(void*)=nullptr;
    Result (*clear_faults)(void*,uint32_t)=nullptr;
    Result (*network)(void*,const uint8_t*,size_t)=nullptr;
    uint32_t (*diagnostic)(void*,unsigned)=nullptr;
    bool (*read_log)(void*,uint32_t&,uint32_t&,uint32_t&)=nullptr;
    void (*command_log)(void*,uint16_t,Result)=nullptr;
    void (*emergency_stop)(void*)=nullptr;
};
class Service {
public:
    Service(Engine& engine,const Hooks& hooks):engine_(engine),hooks_(hooks) {}
    void handle(unsigned transport,const Frame&,Frame& response,uint32_t now_us,const Feedback&);
    // Admission failure executes nothing and does not advance the replay
    // watermark past older queued commands. Completed duplicates still replay.
    void reject_admission(unsigned transport,const Frame&,Frame& response,Result);
    void disconnect(unsigned transport);
    unsigned owner_transport() const { return owner_transport_; }
private:
    struct Connection { uint32_t session=0; Replay replay{}; Config staged{}; };
    Result execute(unsigned,const Frame&,Frame&,uint32_t,const Feedback&);
    Engine& engine_; Hooks hooks_;
    Connection connections_[3]{};
    unsigned owner_transport_=3;
};
}}
