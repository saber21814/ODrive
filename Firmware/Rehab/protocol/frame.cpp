#include "frame.hpp"
#include <algorithm>

namespace rehab { namespace wire {
uint32_t crc32(const uint8_t* bytes,size_t n) {
    uint32_t crc=0xffffffffu;
    while(n--) { crc^=*bytes++; for(unsigned i=0;i<8;++i) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u))); }
    return ~crc;
}
size_t encode(const Frame& f,uint8_t* out,size_t capacity) {
    const size_t n=kHeader+f.length+4;
    if(f.length>kPayload || n>capacity || f.flags>1) return 0;
    out[0]='R';out[1]='H';out[2]=kVersion;out[3]=f.flags;
    put16(out+4,f.length);put16(out+6,f.command);put32(out+8,f.session);put32(out+12,f.sequence);
    std::memcpy(out+kHeader,f.payload,f.length);put32(out+n-4,crc32(out,n-4));return n;
}
void Parser::discard(size_t n) { used_-=n; std::memmove(buffer_,buffer_+n,used_); }
void Parser::scan(Receiver receiver,void* ctx) {
    while(used_>=2) {
        if(buffer_[0]!='R'||buffer_[1]!='H') { discard(1); continue; }
        if(used_<kHeader) return;
        const auto len=u16(buffer_+4);
        if(buffer_[2]!=kVersion || buffer_[3]>1 || len>kPayload) { ++bad_header; discard(1); continue; }
        const size_t n=kHeader+len+4;
        if(used_<n) return;
        if(crc32(buffer_,n-4)!=u32(buffer_+n-4)) { ++bad_crc; discard(1); continue; }
        Frame f;f.flags=buffer_[3];f.length=len;f.command=u16(buffer_+6);f.session=u32(buffer_+8);f.sequence=u32(buffer_+12);
        std::memcpy(f.payload,buffer_+kHeader,len);discard(n);++valid;
        if(receiver) receiver(ctx,f);
    }
}
void Parser::consume(const uint8_t* bytes,size_t n,uint32_t now_ms,Receiver receiver,void* ctx) {
    expire(now_ms);
    if(n)last_ms_=now_ms; // Empty service polls must not keep a partial frame alive.
    while(n--) {
        if(used_==sizeof(buffer_)) { ++bad_header; discard(1); }
        buffer_[used_++]=*bytes++;scan(receiver,ctx);
    }
}
uint32_t Replay::fingerprint(const Frame& f) {
    uint8_t b[kMaxFrame];const auto n=encode(f,b,sizeof(b));return n?crc32(b,n-4):0;
}
Replay::Decision Replay::check(const Frame& f,Frame& cached) const {
    if(!session_ || f.session!=session_ || f.flags) return Decision::Reject;
    for(const auto& e:entries_) if(e.used && e.seq==f.sequence) {
        if(e.hash!=fingerprint(f) || e.request.command!=f.command || e.request.length!=f.length ||
           std::memcmp(e.request.payload,f.payload,f.length)!=0) return Decision::Reject;
        cached=e.response;return Decision::Cached;
    }
    // No sequence wrap within a session. Negotiate a new session at exhaustion.
    return have_last_ && f.sequence<=last_ ? Decision::Reject : Decision::Fresh;
}
void Replay::remember(const Frame& request,const Frame& response) {
    auto& e=entries_[next_];e.used=true;e.seq=request.sequence;e.hash=fingerprint(request);e.request=request;e.response=response;
    next_=(next_+1)%4;last_=request.sequence;have_last_=true;
}
}}
