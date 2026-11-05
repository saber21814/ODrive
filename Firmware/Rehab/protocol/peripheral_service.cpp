#include "peripheral_service.hpp"
#include <limits>

namespace rehab { namespace wire {
void PeripheralService::reject_admission(unsigned port,const Frame& in,Frame& out,
                                         PeripheralResult result) {
    out=Frame{};out.flags=1;out.command=in.command;out.session=in.session;
    out.sequence=in.sequence;out.length=4;
    if(port>=3 || in.flags || in.length>kPayload)result=PeripheralResult::InvalidConfig;
    else if(in.command==uint16_t(Command::Hello)) {
        if(in.length || in.session)result=PeripheralResult::InvalidConfig;
    } else {
        Frame cached;
        const auto admission=connections_[port].replay.check(in,cached);
        if(admission==Replay::Decision::Cached) {out=cached;return;}
        if(admission==Replay::Decision::Reject)result=PeripheralResult::BadSequence;
    }
    put32(out.payload,uint32_t(result));
}

void PeripheralService::disconnect(unsigned port) {
    if(port>=3)return;
    connections_[port].session=0;
    connections_[port].replay.reset(0);
}

void PeripheralService::handle(unsigned port,const Frame& in,Frame& out) {
    out=Frame{};out.flags=1;out.command=in.command;out.session=in.session;
    out.sequence=in.sequence;out.length=4;
    auto result=PeripheralResult::InvalidConfig;
    if(port>=3 || in.flags || in.length>kPayload) {
        put32(out.payload,uint32_t(result));return;
    }
    auto& connection=connections_[port];
    if(in.command==uint16_t(Command::Hello)) {
        if(!in.length && !in.session) {
            uint32_t session=hooks_.new_session?hooks_.new_session(hooks_.ctx):0;
            for(const auto& c:connections_)if(c.session==session)session=0;
            if(!session)result=PeripheralResult::Inhibited;
            else {
                connection.session=session;connection.replay.reset(session);
                out.session=session;
                Writer w{out.payload,sizeof(out.payload)};
                w.word(uint32_t(PeripheralResult::Ok));w.word(kVersion);
                w.word(kPayload);w.word(0x1201);out.length=uint16_t(w.size);
                return;
            }
        }
    } else {
        Frame cached;
        const auto admission=connection.replay.check(in,cached);
        if(admission==Replay::Decision::Cached) {out=cached;return;}
        if(admission==Replay::Decision::Reject)result=PeripheralResult::BadSequence;
        else {
            result=execute(in,out);
            put32(out.payload,uint32_t(result));
            connection.replay.remember(in,out);return;
        }
    }
    put32(out.payload,uint32_t(result));
}

PeripheralResult PeripheralService::execute(const Frame& in,Frame& out) {
    const auto command=static_cast<Command>(in.command);
    Writer w{out.payload,sizeof(out.payload)};w.word(0);
    if(command==Command::Status) {
        if(in.length)return PeripheralResult::InvalidConfig;
        // Legacy 64-byte layout: no training state, owner, calibrated force or
        // mechanical conversion exists in this board/peripheral-only firmware.
        w.word(0);w.word(0);
        w.word(hooks_.diagnostic?hooks_.diagnostic(hooks_.ctx,2):0);
        w.word(0);w.word(0);w.word(0);w.word(0);w.word(UINT32_MAX);
        const float unavailable=std::numeric_limits<float>::quiet_NaN();
        for(unsigned i=0;i<4;++i)w.real(unavailable);
        w.word(0);w.word(0);w.word(0);
        out.length=uint16_t(w.size);return PeripheralResult::Ok;
    }
    if(command==Command::Diagnostics) {
        if(in.length)return PeripheralResult::InvalidConfig;
        static_assert(4+63*4==kPayload,"legacy diagnostics layout");
        for(unsigned i=0;i<63;++i)
            w.word(hooks_.diagnostic?hooks_.diagnostic(hooks_.ctx,i):0);
        out.length=uint16_t(w.size);return PeripheralResult::Ok;
    }
    if(command==Command::Stop) {
        if(in.length)return PeripheralResult::InvalidConfig;
        if(!hooks_.stop)return PeripheralResult::Unsupported;
        hooks_.stop(hooks_.ctx);return PeripheralResult::Ok;
    }
    if(command==Command::SetNetwork)
        return hooks_.network?hooks_.network(hooks_.ctx,in.payload,in.length):PeripheralResult::Unsupported;
    return PeripheralResult::Unsupported;
}
}}
