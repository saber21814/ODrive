#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rehab { namespace wire {
constexpr size_t kPayload = 256, kHeader = 16, kMaxFrame = kHeader + kPayload + 4;
constexpr uint8_t kVersion = 1;
enum class Command : uint16_t {
    Hello=1, Status=2, SetConfig=3, GetConfig=4, CommitConfig=5,
    Acquire=6, Release=7, Enable=8, Start=9, Pause=10, Resume=11,
    Stop=12, ClearFaults=13, Heartbeat=14, Tare=15, Calibrate=16,
    Statistics=17, Diagnostics=18, SetNetwork=19, GetLog=20, Finish=21
};
struct Frame {
    uint8_t flags = 0;
    uint16_t command = 0, length = 0;
    uint32_t session = 0, sequence = 0;
    uint8_t payload[kPayload]{};
};
uint32_t crc32(const uint8_t*, size_t);
size_t encode(const Frame&, uint8_t*, size_t);
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | (uint32_t(u16(p+2)) << 16); }
inline void put16(uint8_t* p, uint16_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); }
inline void put32(uint8_t* p, uint32_t v) { put16(p,uint16_t(v)); put16(p+2,uint16_t(v>>16)); }
struct Writer {
    uint8_t* data; size_t capacity, size=0; bool valid=true;
    bool word(uint32_t v) { if (size+4>capacity) return valid=false; put32(data+size,v); size+=4; return true; }
    bool real(float v) { uint32_t bits; static_assert(sizeof(bits)==sizeof(v), "IEEE float required"); std::memcpy(&bits,&v,4); return word(bits); }
};
class Parser {
public:
    using Receiver = void(*)(void*, const Frame&);
    void consume(const uint8_t*, size_t, uint32_t now_ms, Receiver, void*);
    void reset() { used_=0; }
    void expire(uint32_t now_ms) { if (used_ && uint32_t(now_ms-last_ms_)>250) { used_=0; ++expired; } }
    uint32_t valid=0, bad_crc=0, bad_header=0, expired=0;
private:
    void scan(Receiver, void*);
    void discard(size_t n);
    uint8_t buffer_[kMaxFrame]{};
    size_t used_=0;
    uint32_t last_ms_=0;
};
// At-most-once admission for a negotiated connection, with bounded response
// replay. Older-than-cache sequence numbers are refused, never re-executed.
class Replay {
public:
    enum class Decision { Fresh, Cached, Reject };
    void reset(uint32_t session) { session_=session; have_last_=false; for (auto& e:entries_) e.used=false; next_=0; }
    Decision check(const Frame&, Frame& cached) const;
    void remember(const Frame& request, const Frame& response);
private:
    static uint32_t fingerprint(const Frame&);
    struct Entry { bool used=false; uint32_t seq=0, hash=0; Frame request{}, response{}; };
    Entry entries_[4]{};
    uint32_t session_=0,last_=0; bool have_last_=false; size_t next_=0;
};
}}
