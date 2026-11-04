#include "Rehab/platform/bench_session.hpp"
#include <cassert>
#include <limits>
#include <cstdio>
#include <initializer_list>
using namespace rehab::bench;
int main() {
    for(uint32_t stage=1;stage<=5;++stage)assert(Session::valid(stage,0));
    assert(!Session::valid(0,0) && !Session::valid(6,0));
    for(uint32_t stage : {1u,2u}) {
        assert(Session::valid(stage,1) && Session::valid(stage,4));
        assert(!Session::valid(stage,4.01f) && !Session::valid(stage,-1));
    }
    assert(Session::valid(1,0.1f) && Session::valid(1,0.5f) && !Session::valid(1,0.099f));
    assert(!Session::valid(2,0.5f));
    assert(Session::valid(6,20) && Session::valid(6,-20) && !Session::valid(6,20.01f));
    assert(!Session::valid(6,0) && !Session::valid(6,std::numeric_limits<float>::quiet_NaN()));
    for(uint32_t duration : {200u,1000u,5000u}) {
        Session open;
        assert(open.begin(6,-10,2,7,0xfffffff0u,0,duration));
        assert(open.target_state()==9 && open.duration()==duration);
        open.poll(0xfffffff1u,9,true);
        for(uint32_t elapsed=100;elapsed<duration;elapsed+=100)assert(open.feed(2,7,0xfffffff0u+elapsed));
        open.poll(0xfffffff0u+duration,9,true);assert(open.state==State::Complete);
    }
    for(uint32_t duration : {0u,199u,5001u}) {
        Session open;assert(!open.begin(6,1,2,7,0,0,duration));
    }
    for(bool entered : {false,true}) {
        Session unarmed;assert(unarmed.begin(6,1,2,7,0,0,200));
        if(entered)unarmed.poll(1,9,false);
        unarmed.poll(200,entered?9:1,false);
        assert(unarmed.state==State::Fault && unarmed.reason==Reason::UnexpectedState);
    }
    for(uint32_t phase : {1u,2u,3u,4u,5u,6u}) {
        Session strict;assert(strict.begin(phase,phase==6?1:0,2,7,0,0));
        strict.poll(1,0,false);assert(strict.state==State::Fault && strict.reason==Reason::UnexpectedState);
    }
    assert(!Session::valid(3,0.1f));
    assert(Session::valid(4,-0.3f) && !Session::valid(4,0.31f));
    assert(Session::valid(5,0.5f) && !Session::valid(5,-0.51f));
    assert(!Session::valid(1,std::numeric_limits<float>::quiet_NaN()));
    assert(!Session::valid(1,std::numeric_limits<float>::infinity()));
    // A completion observed at the calibration deadline must never certify it,
    // even with a fresh heartbeat and IDLE/unarmed in that same poll.
    for(uint32_t phase : {1u,2u}) for(uint32_t start : {0u,0xfffffff0u}) {
        Session deadline;
        assert(deadline.begin(phase,0,2,7,start,0));
        deadline.poll(start+1,deadline.target_state(),true);
        for(uint32_t elapsed=100;elapsed<deadline.duration();elapsed+=100)
            assert(deadline.feed(2,7,start+elapsed));
        deadline.poll(start+deadline.duration(),1,false);
        assert(deadline.state==State::Fault && deadline.reason==Reason::Deadline);
    }
    Session s;
    assert(!s.begin(1,0,3,1,0,0) && !s.begin(1,0,0,0,0,0));
    assert(s.begin(1,0,2,7,0xfffffff0u,0));
    assert(!s.begin(2,0,2,7,1,0) && s.epoch==1);
    assert(s.permitted(0x10) && s.remaining(0x10)==7968);
    assert(!s.feed(1,7,0x10) && !s.feed(2,8,0x10));
    assert(s.feed(2,7,0x10));s.poll(0x10,4,true);
    assert(s.state==State::Running && s.entered);
    s.poll(0x11,4,false);assert(s.active()); // Calibration disarm/re-arm inside one state.
    s.poll(0x12,1,false);assert(s.state==State::Complete);
    assert(s.begin(2,4,2,7,100,0) && s.epoch==2);
    assert(!s.feed(2,7,600));s.poll(600,7,false);
    assert(s.state==State::Fault && s.reason==Reason::Heartbeat);
    assert(!s.begin(1,0,2,7,601,0));s.clear();assert(s.state==State::Idle);
    assert(s.begin(1,1,2,7,1000,0));assert(s.feed(2,7,1450));s.poll(1500,1,false);
    assert(s.reason==Reason::UnexpectedState);
    s.clear();assert(s.begin(1,1,2,7,0,0));s.poll(1,4,true);
    for(unsigned t=100;t<8000;t+=100)assert(s.feed(2,7,t));
    s.poll(8000,4,true);assert(s.state==State::Fault && s.reason==Reason::Deadline);
    s.clear();assert(s.begin(3,0,2,7,0,0));s.poll(1,8,true);
    for(unsigned t=100;t<5000;t+=100)assert(s.feed(2,7,t));
    assert(!s.feed(2,7,5000));s.poll(5000,8,true);assert(s.state==State::Complete);
    assert(s.begin(4,0.1f,2,7,6000,0));s.poll(6001,7,false);
    assert(s.reason==Reason::UnexpectedState);
    s.clear();assert(s.begin(5,0.2f,2,7,7000,0));s.stop();assert(s.state==State::Stopped);
    std::puts("PASS bounded bench session bounds, ownership, wrap, expiry, entry and completion");
}
