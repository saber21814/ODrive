#pragma once
#include "stm32_spi_arbiter.hpp"
#include "driver_helpers.hpp"

namespace rehab { namespace drivers {
struct NetworkConfig {
    uint8_t ip[4]{};
    uint8_t gateway[4]{};
    uint8_t netmask[4]{};
    uint16_t port = 39400;
};
class Ch394 {
public:
    enum class State : uint8_t { Disabled, ResetLow, ResetWait, Identify,
        Configure, OpenSocket, WaitCommand, WaitOpen, Monitor, Backoff };
    enum class Error : uint8_t { None, InvalidConfig, Spi, RegisterCheck,
        CommandTimeout, SocketTimeout, IpConflict, BadCount };
    struct Status {
        State state = State::Disabled;
        Error last_error = Error::None;
        uint32_t errors = 0, reconnects = 0, rx_bytes = 0, tx_bytes = 0;
        uint8_t chip_version = 0, socket_state = 0;
        bool link = false, connected = false;
    };
    explicit Ch394(Stm32SpiArbiter& arbiter) : arbiter_(arbiter) {}
    // Starts an asynchronous reset/configuration sequence. Successful return
    // means configuration accepted; status().state reports hardware readiness.
    bool init(const NetworkConfig& config);
    void deinit();
    void poll(uint32_t now_ms);
    size_t receive(uint8_t* data, size_t capacity);
    size_t send(const uint8_t* data, size_t length);
    bool connected() const { return status_.connected; }
    const Status& status() const { return status_; }
    uint32_t errors() const { return status_.errors; }
private:
    bool transfer(uint8_t block, uint16_t address, bool write, uint8_t* bytes, size_t size);
    bool read8(uint8_t block, uint16_t address, uint8_t& value);
    bool write8(uint8_t block, uint16_t address, uint8_t value);
    bool read16(uint16_t address, uint16_t& value);
    bool read16_stable(uint16_t address, uint16_t& value);
    bool write16(uint16_t address, uint16_t value);
    bool command(uint8_t value, State next, uint32_t now_ms);
    bool configure_step();
    bool receive_chunk(uint32_t now_ms);
    bool transmit_chunk(uint32_t now_ms);
    void fail(Error error, uint32_t now_ms);
    void reset_queues();
    Stm32SpiArbiter& arbiter_;
    NetworkConfig config_{};
    Status status_{};
    State command_next_ = State::Monitor;
    uint32_t state_since_ms_ = 0, send_since_ms_ = 0;
    uint8_t configure_index_ = 0, monitor_phase_ = 0;
    uint16_t pending_receive_ = 0;
    bool send_pending_ = false;
    // Single owner (network task). Place the containing object in SRAM, not CCM.
    alignas(4) uint8_t tx_[259]{}, rx_[259]{};
    uint8_t scratch_[256]{};
    ByteQueue<512> receive_queue_{}, transmit_queue_{};
};
} }
