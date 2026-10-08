#include "service.hpp"
#include <algorithm>
#include <cstring>

#ifndef REHAB_ENABLE_APPLICATION
#define REHAB_ENABLE_APPLICATION 1
#endif
namespace rehab { namespace wire {
void Service::reject_admission(unsigned port,const Frame& in,Frame& out,Result result) {
    out=Frame{};out.flags=1;out.command=in.command;out.session=in.session;out.sequence=in.sequence;out.length=4;
    if(port>=3 || in.flags || in.length>kPayload)result=Result::InvalidConfig;
    else if(in.command==uint16_t(Command::Hello)) {
        if(in.length || in.session)result=Result::InvalidConfig;
    } else {
        Frame cached;
        const auto decision=connections_[port].replay.check(in,cached);
        if(decision==Replay::Decision::Cached){out=cached;return;}
        if(decision==Replay::Decision::Reject)result=Result::BadSequence;
    }
    put32(out.payload,uint32_t(result));
    if(hooks_.command_log)hooks_.command_log(hooks_.ctx,in.command,result);
}
void Service::disconnect(unsigned transport) {
    if(transport>=3)return;
    if(owner_transport_==transport && engine_.status().session) engine_.trip(FaultHeartbeat);
    connections_[transport].session=0;connections_[transport].replay.reset(0);
    // Keep the faulted lease. A new connection must clear/release it explicitly
    // through the recovery path; it never resumes the interrupted motion.
}
void Service::handle(unsigned port,const Frame& in,Frame& out,uint32_t now,const Feedback& fb) {
    out=Frame{};out.flags=1;out.command=in.command;out.session=in.session;out.sequence=in.sequence;out.length=4;
    Result result=Result::InvalidConfig;
    if(port>=3 || in.flags || in.length>kPayload) { put32(out.payload,uint32_t(result));return; }
    auto& connection=connections_[port];
    if(in.command==uint16_t(Command::Hello)) {
        if(in.length || in.session) result=Result::InvalidConfig;
        else if(engine_.status().session && owner_transport_==port && connection.session) result=Result::Busy;
        else {
            uint32_t session=hooks_.new_session?hooks_.new_session(hooks_.ctx):0;
            // Never reuse an active or disconnected lease's nonce.
            if(session==engine_.status().session)session=0;
            for(const auto& c:connections_)if(c.session==session)session=0;
            if(!session) result=Result::Inhibited;
            else {
                connection.session=session;connection.replay.reset(session);connection.staged=engine_.config();
                out.session=session;Writer w{out.payload,sizeof(out.payload)};
                w.word(uint32_t(Result::Ok));w.word(kVersion);w.word(kPayload);w.word(0x1201);
                out.length=uint16_t(w.size);return;
            }
        }
    } else {
        Frame cached;
        const auto decision=connection.replay.check(in,cached);
        if(decision==Replay::Decision::Cached) { out=cached;return; }
        if(decision==Replay::Decision::Reject) result=Result::BadSequence;
        else {
            result=execute(port,in,out,now,fb);
            put32(out.payload,uint32_t(result));connection.replay.remember(in,out);
            if(hooks_.command_log)hooks_.command_log(hooks_.ctx,in.command,result);
            return;
        }
    }
    put32(out.payload,uint32_t(result));
}
Result Service::execute(unsigned port,const Frame& in,Frame& out,uint32_t now,const Feedback& fb) {
    auto& connection=connections_[port];
    const auto command=static_cast<Command>(in.command);
    const auto& s=engine_.status();
    Writer w{out.payload,sizeof(out.payload)};w.word(0);
    if(command==Command::Status) {
        if(in.length)return Result::InvalidConfig;
        w.word(unsigned(s.state));w.word(unsigned(s.mode));w.word(s.faults);w.word(s.session);w.word(s.epoch);
        w.word(s.config_valid);w.word(s.force_valid);w.word(s.sample_age_us);
        w.real(s.length_m);w.real(s.speed_m_s);w.real(s.force_n);w.real(s.target.torque_nm);
        w.word(s.target.enable);w.word(s.rfd_valid);w.word(hooks_.motion_allowed && hooks_.motion_allowed(hooks_.ctx));
        out.length=uint16_t(w.size);return Result::Ok;
    }
    if(command==Command::Diagnostics) {
        if(in.length)return Result::InvalidConfig;
        static_assert(4+4*kDiagnosticWords<=kPayload,"diagnostics must fit one frame");
        for(unsigned i=0;i<kDiagnosticWords;++i)w.word(hooks_.diagnostic?hooks_.diagnostic(hooks_.ctx,i):0);
        out.length=uint16_t(w.size);return Result::Ok;
    }
    if(command==Command::GetLog) {
        if(in.length)return Result::InvalidConfig;
        uint32_t ts=0,code=0,value=0;bool found=hooks_.read_log && hooks_.read_log(hooks_.ctx,ts,code,value);
        w.word(found);w.word(ts);w.word(code);w.word(value);out.length=uint16_t(w.size);return Result::Ok;
    }
    // Any negotiated, replay-checked connection can stop, including an observer.
    // The hardware hook latches/disarms independently of state-machine ownership.
    if(command==Command::Stop) {
        if(in.length)return Result::InvalidConfig;
        if(hooks_.emergency_stop)hooks_.emergency_stop(hooks_.ctx);
        engine_.trip(FaultStop);
        return Result::Ok;
    }
    if(command==Command::SetNetwork) {
        if(!fb.idle || fb.armed || s.session)return Result::Busy;
        return hooks_.network?hooks_.network(hooks_.ctx,in.payload,in.length):Result::Unsupported;
    }
#if !REHAB_ENABLE_APPLICATION
    (void)connection;(void)now;
    return Result::Unsupported;
#else
    if(command==Command::GetConfig) {
        if(in.length!=8)return Result::InvalidConfig;
        const uint16_t first=u16(in.payload),count=u16(in.payload+2);
        const uint32_t staged=u32(in.payload+4);
        if(!count || count>31 || first+count>config_field_count() || staged>1)return Result::InvalidConfig;
        const Config& c=staged?connection.staged:engine_.config();
        for(uint16_t i=first;i<first+count;++i) { uint16_t type;uint32_t bits;get_config_field(c,i,type,bits);w.word(uint32_t(i)|(uint32_t(type)<<16));w.word(bits); }
        out.length=uint16_t(w.size);return Result::Ok;
    }
    if(command==Command::SetConfig) {
        if(!fb.idle || fb.armed || (s.session && s.session!=in.session))return Result::Busy;
        if(!in.length || in.length%8)return Result::InvalidConfig;
        Config candidate=connection.staged;
        for(unsigned i=0;i<in.length;i+=8) {
            const Result r=set_config_field(candidate,u16(in.payload+i),u16(in.payload+i+2),u32(in.payload+i+4));
            if(r!=Result::Ok)return r;
        }
        connection.staged=candidate;return Result::Ok;
    }
    if(command==Command::CommitConfig) {
        if(in.length)return Result::InvalidConfig;
        if(!fb.idle || fb.armed || (s.session && s.session!=in.session))return Result::Busy;
        return engine_.configure(connection.staged);
    }
    if(command==Command::Acquire) {
        if(in.length)return Result::InvalidConfig;
        if(s.session)return Result::Busy;
        const Result r=hooks_.acquire?hooks_.acquire(hooks_.ctx,in.session,fb):engine_.acquire(in.session,fb);
        if(r==Result::Ok) {owner_transport_=port;engine_.heartbeat(in.session,now);}
        return r;
    }
    if(command==Command::Statistics) {
#if !REHAB_ENABLE_STATISTICS
        return Result::Unsupported;
#else
        if(in.length)return Result::InvalidConfig;
        const auto& stats=s.stats;
        w.word(stats.repetitions);w.word(stats.force_samples);w.real(stats.elapsed_s);
        w.real(stats.peak_force_n);w.real(stats.mean_force_n);w.real(stats.peak_speed_m_s);w.real(stats.mean_speed_m_s);
        w.real(stats.power_w);w.real(stats.positive_energy_j);w.real(stats.negative_energy_j);
        w.real(stats.rfd_n_s);w.real(stats.peak_rfd_n_s);w.word(stats.fitness_result_valid);out.length=uint16_t(w.size);return Result::Ok;
#endif
    }
    // Recovery of a disconnected lease requires a new session, actual IDLE,
    // explicit CLEAR, then RELEASE. Never bind a new session to a running axis.
    if(command==Command::ClearFaults && s.session && owner_transport_==port &&
       s.session!=in.session && s.state==State::Fault && fb.idle && !fb.armed && !fb.stop) {
        if(in.length)return Result::InvalidConfig;
        const uint32_t old=s.session;
        Result r=hooks_.clear_faults?hooks_.clear_faults(hooks_.ctx,old):engine_.clear_faults(old);
        if(r!=Result::Ok)return r;
        r=hooks_.release?hooks_.release(hooks_.ctx,old,fb):engine_.release(old,fb);
        if(r==Result::Ok)owner_transport_=3;
        return r;
    }
    if(owner_transport_!=port || s.session!=in.session)return Result::NotOwner;
    if(command!=Command::Start && command!=Command::Calibrate && in.length)return Result::InvalidConfig;
    switch(command) {
        case Command::Release: {
            const Result r=hooks_.release?hooks_.release(hooks_.ctx,in.session,fb):engine_.release(in.session,fb);
            if(r==Result::Ok)owner_transport_=3;
            return r;
        }
        case Command::Enable:
            if(!hooks_.motion_allowed || !hooks_.motion_allowed(hooks_.ctx))return Result::Inhibited;
            return engine_.enable(in.session);
        case Command::Start:
            if(in.length!=4 || u32(in.payload)>uint32_t(Mode::Release))return Result::InvalidConfig;
            if(static_cast<Mode>(u32(in.payload))==Mode::Release)return Result::Unsupported;
            if(!hooks_.motion_allowed || !hooks_.motion_allowed(hooks_.ctx))return Result::Inhibited;
            return engine_.start(in.session,static_cast<Mode>(u32(in.payload)));
        case Command::Pause:return engine_.pause(in.session);
        case Command::Resume:
            if(!hooks_.motion_allowed || !hooks_.motion_allowed(hooks_.ctx))return Result::Inhibited;
            return engine_.resume(in.session);
        case Command::Finish:return engine_.stop(in.session);
        case Command::ClearFaults:return hooks_.clear_faults?hooks_.clear_faults(hooks_.ctx,in.session):engine_.clear_faults(in.session);
        case Command::Heartbeat:return engine_.heartbeat(in.session,now);
        case Command::Tare: {
            const Result r=engine_.tare(in.session);
            if(r==Result::Ok)connection.staged.force_offset_counts=engine_.config().force_offset_counts;
            return r;
        }
        case Command::Calibrate: {
            if(in.length!=4)return Result::InvalidConfig;
            float force;std::memcpy(&force,in.payload,4);const Result r=engine_.calibrate(in.session,force);
            if(r==Result::Ok) {
                connection.staged.force_offset_counts=engine_.config().force_offset_counts;
                connection.staged.force_scale_n_per_count=engine_.config().force_scale_n_per_count;
            }
            return r;
        }
        default:return Result::Unsupported;
    }
#endif
}
}}
