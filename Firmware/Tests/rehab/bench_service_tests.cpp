#include "Rehab/protocol/service.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace rehab;
using namespace rehab::wire;
struct Fixture {
    Engine engine;
    Feedback fb{};
    uint32_t nonce=10, feeds=0, starts=0, stops=0, lost=0, lost_session=0;
    bool active=false, overflow=false, refuse_nonce=false;
    Hooks hooks;
    Service service;
    Parser parser;
    Frame response{};
    unsigned responses=0;
    static Hooks make(Fixture* self, bool installed) {
        Hooks h;h.ctx=self;
        h.new_session=[](void* c){auto& f=*static_cast<Fixture*>(c);return f.refuse_nonce?0:++f.nonce;};
        h.connection_lost=[](void* c,unsigned port,uint32_t session){
            auto& f=*static_cast<Fixture*>(c);assert(port<3 && session);++f.lost;f.lost_session=session;f.active=false;
        };
        if(installed)h.bench=[](void* c,unsigned port,uint16_t cmd,uint32_t session,const uint8_t*,size_t,Writer& w){
            auto& f=*static_cast<Fixture*>(c);assert(port<3 && session);
            if(cmd==22){++f.starts;f.active=true;}
            if(cmd==23)++f.feeds;
            if(cmd==24){++f.stops;f.active=false;}
            for(unsigned i=0;i<(f.overflow?70u:47u);++i)w.word(i);
            return Result::Ok;
        };
        return h;
    }
    explicit Fixture(bool installed=true):hooks(make(this,installed)),service(engine,hooks){fb.idle=true;}
    Frame handle(const Frame& in,unsigned port=2){Frame out;service.handle(port,in,out,1000,fb);return out;}
    uint32_t hello(unsigned port=2){Frame in;in.command=1;in.sequence=1;auto out=handle(in,port);assert(u32(out.payload)==0 && out.length==16);return out.session;}
    static void receive(void* c,const Frame& in){auto& f=*static_cast<Fixture*>(c);f.response=f.handle(in);++f.responses;}
};
Frame request(unsigned cmd,uint32_t session,uint32_t seq,unsigned length=0){
    Frame f;f.command=uint16_t(cmd);f.session=session;f.sequence=seq;f.length=uint16_t(length);
    if(cmd==22 && length==8){put32(f.payload,1);put32(f.payload+4,0);}
    return f;
}
Result result(const Frame& f){return static_cast<Result>(u32(f.payload));}
void sessions_replay(){
    Fixture f;auto session=f.hello();
    auto start=request(22,session,2,8);auto out=f.handle(start);
    assert(result(out)==Result::Ok && out.length==192 && f.starts==1 && f.active);
    auto duplicate=f.handle(start);assert(duplicate.length==out.length && !std::memcmp(out.payload,duplicate.payload,out.length));
    assert(f.starts==1);
    auto feed=request(23,session,3);assert(result(f.handle(feed))==Result::Ok && f.feeds==1);
    assert(result(f.handle(feed))==Result::Ok && f.feeds==1);
    auto conflict=start;conflict.payload[0]=2;assert(result(f.handle(conflict))==Result::BadSequence && f.starts==1);
    auto next=f.hello();assert(next!=session && f.lost==1 && f.lost_session==session && !f.active);
    assert(result(f.handle(request(23,session,4)))==Result::BadSequence && f.feeds==1);
    assert(result(f.handle(request(22,next,2,8)))==Result::Ok && f.active);
    auto observer=f.hello(0);assert(f.active);
    assert(result(f.handle(request(24,observer,2),0))==Result::Ok && !f.active && f.stops==1);
    assert(result(f.handle(request(24,observer,2),0))==Result::Ok && f.stops==1);
    f.service.disconnect(2);assert(f.lost==2 && f.lost_session==next);
    f.service.disconnect(2);f.service.disconnect(99);assert(f.lost==2);
    assert(result(f.handle(request(23,next,3)))==Result::BadSequence);
}
void hello_refusal(){
    Fixture f;auto session=f.hello();f.handle(request(22,session,2,8));
    auto bad=request(1,session,3);assert(result(f.handle(bad))==Result::InvalidConfig && !f.lost && f.active);
    bad=request(1,0,3,1);assert(result(f.handle(bad))==Result::InvalidConfig && !f.lost && f.active);
    f.refuse_nonce=true;bad=request(1,0,3);assert(result(f.handle(bad))==Result::Inhibited && !f.lost && f.active);
    f.refuse_nonce=false;
#if REHAB_ENABLE_APPLICATION
    assert(result(f.handle(request(6,session,3)))==Result::Ok);
    assert(result(f.handle(request(1,0,4)))==Result::Busy && !f.lost && f.active);
#endif
}
void validation(){
    Fixture f;auto s=f.hello();unsigned seq=1;
    for(unsigned cmd=22;cmd<=26;++cmd){
        auto bad=request(cmd,s,++seq,cmd==22?0:1);assert(result(f.handle(bad))==Result::InvalidConfig);
    }
    assert(f.starts==0 && f.feeds==0 && f.stops==0);
    Fixture absent(false);s=absent.hello();
    for(unsigned cmd=22;cmd<=26;++cmd)assert(result(absent.handle(request(cmd,s,++seq,cmd==22?8:0)))==Result::Unsupported);
    s=f.hello();f.overflow=true;auto out=f.handle(request(25,s,2));
    assert(result(out)==Result::InvalidConfig && out.length==4);
    // Legacy binary Diagnostics must still contain result + exactly 63 words.
    out=f.handle(request(18,s,3));assert(result(out)==Result::Ok && out.length==256);
    assert(kDiagnosticWords==63);
}
void framing(){
    Fixture f;auto s=f.hello();auto start=request(22,s,2,8);
    uint8_t bytes[kMaxFrame];auto n=encode(start,bytes,sizeof(bytes));assert(n);
    bytes[n-1]^=1;f.parser.consume(bytes,n,0,Fixture::receive,&f);assert(!f.responses && !f.starts && f.parser.bad_crc==1);
    bytes[n-1]^=1;f.parser.consume(bytes,5,1,Fixture::receive,&f);assert(!f.responses);
    f.parser.consume(bytes+5,n-5,2,Fixture::receive,&f);assert(f.responses==1 && f.starts==1);
    f.parser.consume(bytes,n,3,Fixture::receive,&f);assert(f.responses==2 && f.starts==1);
    // An admitted observer stop is idempotent even when replayed.
    auto stop=request(24,s,3);n=encode(stop,bytes,sizeof(bytes));f.parser.consume(bytes,n,4,Fixture::receive,&f);
    assert(!f.active && f.stops==1);
}
int main(){sessions_replay();hello_refusal();validation();framing();std::puts("PASS bench service: sessions/replay/Hello/disconnect/length/null hook/framing");}
