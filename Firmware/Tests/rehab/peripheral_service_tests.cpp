#include "Rehab/protocol/peripheral_service.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace rehab::wire;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);} } while(0)
struct Fixture {
    uint32_t nonce=100;
    unsigned stops=0,configs=0;
    PeripheralResult network_result=PeripheralResult::Ok;
    PeripheralHooks hooks{
        this,[](void* p){return ++static_cast<Fixture*>(p)->nonce;},
        [](void*,unsigned i){return uint32_t(1000+i);},
        [](void* p,const uint8_t*,size_t n){
            auto& f=*static_cast<Fixture*>(p);++f.configs;
            return n==14?f.network_result:PeripheralResult::InvalidConfig;
        },
        [](void* p){++static_cast<Fixture*>(p)->stops;}
    };
    PeripheralService service{hooks};
    Parser parser;
    std::vector<Frame> replies;
    void feed(const uint8_t* p,size_t n,uint32_t now=1) {
        parser.consume(p,n,now,[](void* p,const Frame& request){
            auto& f=*static_cast<Fixture*>(p);Frame response;
            f.service.handle(2,request,response);f.replies.push_back(response);
        },this);
    }
    Frame send(const Frame& in,unsigned port=2) {
        Frame out;service.handle(port,in,out);return out;
    }
    uint32_t hello(unsigned port=2) {
        Frame in;in.command=1;in.sequence=1;
        const auto out=send(in,port);
        CHECK(out.flags==1 && out.command==1 && out.sequence==1);
        CHECK(out.length==16 && u32(out.payload)==0 && out.session!=0);
        CHECK(u32(out.payload+4)==1 && u32(out.payload+8)==256 && u32(out.payload+12)==0x1201);
        return out.session;
    }
};
static Frame request(uint16_t command,uint32_t session,uint32_t seq) {
    Frame f;f.command=command;f.session=session;f.sequence=seq;return f;
}
static bool result(const Frame& f,PeripheralResult r) {return u32(f.payload)==uint32_t(r);}
static float real(const uint8_t* p) {float f;std::memcpy(&f,p,4);return f;}
static bool same(const Frame& a,const Frame& b) {
    uint8_t aa[kMaxFrame],bb[kMaxFrame];const auto n=encode(a,aa,sizeof(aa));
    return n==encode(b,bb,sizeof(bb)) && std::memcmp(aa,bb,n)==0;
}
int main() {
    Fixture f;const auto session=f.hello();
    auto status=f.send(request(2,session,2));
    CHECK(result(status,PeripheralResult::Ok) && status.length==64);
    CHECK(u32(status.payload+12)==1002 && u32(status.payload+32)==UINT32_MAX);
    for(unsigned i:{1u,2u,4u,5u,6u,7u,13u,14u,15u})CHECK(u32(status.payload+4*i)==0);
    for(unsigned i=9;i<13;++i)CHECK(std::isnan(real(status.payload+4*i)));
    auto diag=f.send(request(18,session,3));
    CHECK(result(diag,PeripheralResult::Ok) && diag.length==256);
    for(unsigned i=0;i<63;++i)CHECK(u32(diag.payload+4+4*i)==1000+i);
    CHECK(f.stops==0 && f.configs==0);

    auto stop=request(12,session,4);auto first=f.send(stop);
    CHECK(result(first,PeripheralResult::Ok) && f.stops==1);
    CHECK(same(first,f.send(stop)) && f.stops==1);
    stop.length=1;
    CHECK(result(f.send(stop),PeripheralResult::BadSequence) && f.stops==1);
    stop.sequence=5;
    CHECK(result(f.send(stop),PeripheralResult::InvalidConfig) && f.stops==1);
    auto net=request(19,session,6);net.length=14;
    first=f.send(net);CHECK(result(first,PeripheralResult::Ok) && f.configs==1);
    CHECK(same(first,f.send(net)) && f.configs==1);
    net.sequence=7;f.network_result=PeripheralResult::Busy;
    CHECK(result(f.send(net),PeripheralResult::Busy) && f.configs==2);
    CHECK(result(f.send(net),PeripheralResult::Busy) && f.configs==2);
    uint32_t seq=8;
    for(uint16_t cmd=3;cmd<=26;++cmd) {
        if(cmd==12 || cmd==18 || cmd==19)continue;
        CHECK(result(f.send(request(cmd,session,seq++)),PeripheralResult::Unsupported));
    }
    CHECK(result(f.send(request(65535,session,seq++)),PeripheralResult::Unsupported));
    CHECK(f.stops==1 && f.configs==2);
    CHECK(result(f.send(request(12,session,4)),PeripheralResult::BadSequence));
    auto invalid=request(2,session,seq++);invalid.flags=1;
    CHECK(result(f.send(invalid),PeripheralResult::InvalidConfig));
    invalid.flags=0;invalid.length=257;
    CHECK(result(f.send(invalid),PeripheralResult::InvalidConfig));
    CHECK(result(f.send(request(12,session,seq++),3),PeripheralResult::InvalidConfig));
    for(auto cmd:{2,18}) {
        invalid=request(uint16_t(cmd),session,seq++);invalid.length=1;
        CHECK(result(f.send(invalid),PeripheralResult::InvalidConfig));
    }
    auto badhello=request(1,session,seq++);
    CHECK(result(f.send(badhello),PeripheralResult::InvalidConfig));
    badhello.session=0;badhello.length=1;
    CHECK(result(f.send(badhello),PeripheralResult::InvalidConfig));

    const auto other=f.hello(0);CHECK(other!=session);
    f.nonce=other-1;
    CHECK(result(f.send(request(1,0,1),1),PeripheralResult::Inhibited));
    f.nonce=UINT32_MAX;
    CHECK(result(f.send(request(1,0,1),1),PeripheralResult::Inhibited));
    f.nonce=1000;
    for(unsigned i=0;i<3;++i) {
        const auto old=i?f.replies.back().session:session;
        f.service.disconnect(2);
        CHECK(result(f.send(request(12,old,1)),PeripheralResult::BadSequence));
        const auto next=f.hello();CHECK(next!=old);
        CHECK(result(f.send(request(18,old,2)),PeripheralResult::BadSequence));
        f.replies.push_back(f.send(request(2,next,2)));
        CHECK(result(f.replies.back(),PeripheralResult::Ok));
    }
    f.service.disconnect(99);CHECK(f.stops==1);

    Fixture stream;uint8_t bytes[kMaxFrame];auto hello=request(1,0,1);
    const auto hn=encode(hello,bytes,sizeof(bytes));
    for(size_t i=0;i<hn;++i)stream.feed(bytes+i,1);
    CHECK(stream.replies.size()==1 && stream.parser.valid==1);
    const auto ss=stream.replies[0].session;
    std::vector<uint8_t> combined;
    for(uint16_t cmd:{uint16_t(2),uint16_t(18)}) {
        const auto n=encode(request(cmd,ss,cmd),bytes,sizeof(bytes));
        combined.insert(combined.end(),bytes,bytes+n);
    }
    stream.feed(combined.data(),combined.size());
    CHECK(stream.replies.size()==3 && stream.replies[1].length==64 && stream.replies[2].length==256);
    auto corrupt=request(12,ss,19);auto n=encode(corrupt,bytes,sizeof(bytes));bytes[n-1]^=0x80;
    stream.feed(bytes,n);CHECK(stream.stops==0 && stream.parser.bad_crc==1);
    n=encode(corrupt,bytes,sizeof(bytes));stream.feed(bytes,n);
    CHECK(stream.stops==1 && result(stream.replies.back(),PeripheralResult::Ok));
    stream.feed(bytes,n);CHECK(stream.stops==1);
    const auto dn=encode(request(18,ss,20),bytes,sizeof(bytes));
    stream.feed(bytes,7,10);stream.parser.expire(261);
    CHECK(stream.parser.expired==1);
    stream.feed(bytes,dn,262);CHECK(stream.replies.back().length==256);
    for(const auto& reply:stream.replies) {
        const auto len=encode(reply,bytes,sizeof(bytes));
        CHECK(len==size_t(reply.length)+20 && u32(bytes+len-4)==crc32(bytes,len-4));
    }
    PeripheralService nohooks{PeripheralHooks{}};
    Frame out;nohooks.handle(0,hello,out);CHECK(result(out,PeripheralResult::Inhibited));
    Fixture admission;const auto as=admission.hello();
    auto completed=request(12,as,2);
    const auto completed_reply=admission.send(completed);
    CHECK(admission.stops==1);
    for(auto failure:{PeripheralResult::QueueFull,PeripheralResult::Stale}) {
        admission.service.reject_admission(2,completed,out,failure);
        CHECK(same(out,completed_reply) && admission.stops==1);
        auto wrong_session=completed;wrong_session.session=as+1;
        admission.service.reject_admission(2,wrong_session,out,failure);
        CHECK(result(out,PeripheralResult::BadSequence));
        auto changed=completed;changed.command=18;
        admission.service.reject_admission(2,changed,out,failure);
        CHECK(result(out,PeripheralResult::BadSequence));
        admission.service.reject_admission(3,completed,out,failure);
        CHECK(result(out,PeripheralResult::InvalidConfig));
        auto oversized=completed;oversized.length=257;
        admission.service.reject_admission(2,oversized,out,failure);
        CHECK(result(out,PeripheralResult::InvalidConfig));
        auto flagged=completed;flagged.flags=1;
        admission.service.reject_admission(2,flagged,out,failure);
        CHECK(result(out,PeripheralResult::InvalidConfig));
        admission.service.reject_admission(2,hello,out,failure);
        CHECK(result(out,failure) && out.length==4 && out.session==0);
        auto malformed_hello=hello;malformed_hello.session=as;
        admission.service.reject_admission(2,malformed_hello,out,failure);
        CHECK(result(out,PeripheralResult::InvalidConfig));
        auto pending=request(19,as,100);pending.length=14;
        admission.service.reject_admission(2,pending,out,failure);
        CHECK(result(out,failure) && admission.configs==0);
    }
    // Rejected sequence 100 must not invalidate a pending older command, nor
    // become a cached error that prevents the same fresh request being retried.
    CHECK(result(admission.send(request(18,as,3)),PeripheralResult::Ok));
    auto retry=request(19,as,100);retry.length=14;
    CHECK(result(admission.send(retry),PeripheralResult::Ok) && admission.configs==1);
    admission.service.reject_admission(2,retry,out,PeripheralResult::QueueFull);
    CHECK(result(out,PeripheralResult::Ok) && admission.configs==1);
    std::printf("PASS: peripheral service (%u checks)\n",checks);
}
