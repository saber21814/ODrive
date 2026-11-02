#pragma once
// Transport carrier only. Runtime/Service network admission and framing remain
// production code; electrical CH394 behavior is covered separately.
namespace rehab { namespace drivers {
struct NetworkConfig {
    uint8_t ip[4]{}, gateway[4]{}, netmask[4]{};
    uint16_t port=39400;
};
class Ch394 {
public:
    struct Status {
        unsigned state=0, last_error=0, chip_version=0, reconnects=0;
        bool link=false, connected=false;
    };
    explicit Ch394(MockSpiArbiter&) {}
    bool init(const NetworkConfig&) { return true; }
    void deinit() {}
    void poll(uint32_t) {}
    size_t receive(uint8_t*,size_t) { return 0; }
    size_t send(const uint8_t*,size_t) { return 0; }
    bool connected() const { return false; }
    Status status() const { return {}; }
    uint32_t errors() const { return 0; }
};
} }
