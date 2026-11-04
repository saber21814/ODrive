#pragma once
#include <cstdint>
#include <cmath>
#ifndef REHAB_BENCH
#define REHAB_BENCH 0
#endif
namespace rehab { namespace bench {
enum class State : uint32_t { Idle, Pending, Running, Complete, Stopped, Fault };
enum class Reason : uint32_t { None, Heartbeat, Deadline, Disconnected, Stop, Native,
    Bus, Temperature, Adc, Speed, Travel, UnexpectedState, Prerequisite, Position, Numeric };
// Single-core ISR/task sharing: task callers must exclude interrupts for every
// access. No allocation, waits, hardware access or automatic next phase.
struct Session {
    State state=State::Idle;
    Reason reason=Reason::None;
    uint32_t stage=0, epoch=0, port=3, session=0, started_ms=0, fed_ms=0;
    bool entered=false;
    float argument=0, start_position=0;
    uint32_t open_loop_ms=1000;
    bool active() const { return state==State::Pending || state==State::Running; }
    uint32_t target_state() const { return stage==1?4:stage==2?7:stage==6?9:8; }
    uint32_t duration() const { return stage==1?8000:stage==2?15000:stage==3?5000:stage==6?open_loop_ms:10000; }
    bool permitted(uint32_t now) const { return active() && uint32_t(now-fed_ms)<500 && uint32_t(now-started_ms)<duration(); }
    uint32_t remaining(uint32_t now) const {
        const uint32_t age=now-started_ms;
        return active() && age<duration()?duration()-age:0;
    }
    static bool valid(uint32_t phase,float value) {
        return std::isfinite(value) && phase>=1 && phase<=6 &&
            (phase<=2?(value==0.0f || (value>=(phase==1?0.1f:1.0f) && value<=4.0f)):
             phase==3?value==0.0f:phase==6?(value!=0.0f && std::fabs(value)<=20.0f):
             std::fabs(value)<=(phase==4?0.3f:0.5f));
    }
    bool begin(uint32_t phase,float value,uint32_t transport,uint32_t nonce,uint32_t now,float position,uint32_t run_ms=1000) {
        if(active() || state==State::Fault || !valid(phase,value) || transport>=3 || !nonce || !std::isfinite(position) ||
           (phase==6 && (run_ms<200 || run_ms>5000)))return false;
        stage=phase;argument=value;port=transport;session=nonce;started_ms=fed_ms=now;
        start_position=position;open_loop_ms=run_ms;entered=false;reason=Reason::None;state=State::Pending;++epoch;
        return true;
    }
    bool owns(uint32_t transport,uint32_t nonce) const { return port==transport && session==nonce && nonce; }
    void fail(Reason why) { state=State::Fault;reason=why; }
    void stop() { if(active()){state=State::Stopped;reason=Reason::Stop;} }
    void clear() { if(!active()){state=State::Idle;reason=Reason::None;port=3;session=0;} }
    bool feed(uint32_t transport,uint32_t nonce,uint32_t now) {
        if(!owns(transport,nonce))return false;
        if(!active())return state==State::Complete || state==State::Stopped;
        // Completion and physical cleanup belong to poll/the adapter, never feed.
        if(!permitted(now))return false;
        fed_ms=now;return true;
    }
    void poll(uint32_t now,uint32_t axis_state,bool armed) {
        if(!active())return;
        if(uint32_t(now-fed_ms)>=500){fail(Reason::Heartbeat);return;}
        // A late IDLE observation must not certify an over-deadline calibration.
        if(stage<=2 && uint32_t(now-started_ms)>=duration()){fail(Reason::Deadline);return;}
        if(axis_state!=1 && axis_state!=target_state()){fail(Reason::UnexpectedState);return;}
        if(axis_state==target_state()){entered=true;state=State::Running;}
        if(!entered && uint32_t(now-started_ms)>=500){fail(Reason::UnexpectedState);return;}
        if(entered && axis_state==1 && !armed){
            if(stage<=2)state=State::Complete;else fail(Reason::UnexpectedState);
            return;
        }
        if(uint32_t(now-started_ms)>=duration()){
            if(stage<=2)fail(Reason::Deadline);
            else if(stage==6 && (!entered || !armed))fail(Reason::UnexpectedState);
            else state=State::Complete;
        }
    }
};
}}
