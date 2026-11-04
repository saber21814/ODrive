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
#if defined(REHAB_V13)
        struct TimeoutSnapshot {
            uint32_t sequence=0,failed_state=0,command=0,valid_mask=0;
            uint32_t sn_mode=0,sn_ctrl=0,sn_int=0,sn_sta=0,sn_port=0;
            uint32_t phy_cfg=0,mode_reg=0,chip_config=0,reg_page=0;
        } timeout;
#endif
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
