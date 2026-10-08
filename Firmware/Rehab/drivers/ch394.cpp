#include "ch394.hpp"
#include "stm32_system.h"
#include <cstring>

// Register offsets and framing below are from WCH CH394Q DS1 v1.2,
// sections 5, 6, 7, 10.4, 12.2. This is not a CH395 command protocol.
namespace rehab { namespace drivers {
namespace {
constexpr uint8_t common = 0x00, socket0 = 0x08, tx_buffer = 0x10, rx_buffer = 0x18;
constexpr uint16_t gwip = 0x0001, smip = 0x0005, source_ip = 0x000f;
constexpr uint16_t gint = 0x0015, phy_cfg = 0x002e, chipv = 0x0039, chip_config = 0x003e;
constexpr uint16_t sn_mode = 0x0000, sn_ctrl = 0x0001, sn_int = 0x0002, sn_sta = 0x0003;
constexpr uint16_t sn_port = 0x0004, sn_rx_size = 0x001e, sn_tx_size = 0x001f;
constexpr uint16_t sn_tx_free = 0x0020, sn_tx_wr = 0x0024, sn_rx_count = 0x0026, sn_rx_rd = 0x0028;
const SPI_InitTypeDef spi_config = {
    .Mode = SPI_MODE_MASTER, .Direction = SPI_DIRECTION_2LINES, .DataSize = SPI_DATASIZE_8BIT,
    .CLKPolarity = SPI_POLARITY_LOW, .CLKPhase = SPI_PHASE_1EDGE, .NSS = SPI_NSS_SOFT,
    .BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16, .FirstBit = SPI_FIRSTBIT_MSB,
    .TIMode = SPI_TIMODE_DISABLE, .CRCCalculation = SPI_CRCCALCULATION_DISABLE, .CRCPolynomial = 10,
};
Stm32Gpio cs{GPIOC, GPIO_PIN_2}, reset{GPIOA, GPIO_PIN_4}, interrupt_pin{GPIOC, GPIO_PIN_8};
bool valid_config(const NetworkConfig& c) {
    const uint32_t mask = (uint32_t(c.netmask[0]) << 24) | (uint32_t(c.netmask[1]) << 16)
        | (uint32_t(c.netmask[2]) << 8) | c.netmask[3];
    const uint32_t host_mask = ~mask;
    const uint32_t ip = (uint32_t(c.ip[0]) << 24) | (uint32_t(c.ip[1]) << 16)
        | (uint32_t(c.ip[2]) << 8) | c.ip[3];
    return c.port && c.ip[0] && c.ip[0] < 224 && c.ip[0] != 127
        && mask && host_mask > 1 && !(host_mask & (host_mask + 1))
        && (ip & host_mask) && ((ip & host_mask) != host_mask);
}
}
bool Ch394::init(const NetworkConfig& config) {
    if (!valid_config(config)) { status_.last_error = Error::InvalidConfig; return false; }
    if (status_.state != State::Disabled) deinit();
    config_ = config;
    status_ = Status{};
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOC_CLK_ENABLE();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    status_.state = State::ResetLow;
    cs.write(true); reset.write(false);
    if (!cs.config(GPIO_MODE_OUTPUT_PP, GPIO_NOPULL)
        || !reset.config(GPIO_MODE_OUTPUT_PP, GPIO_NOPULL)
        || !interrupt_pin.config(GPIO_MODE_INPUT, GPIO_PULLUP)) {
        deinit(); return false;
    }
    reset_queues();
    status_.state = State::ResetLow;
    state_since_ms_ = HAL_GetTick();
    return true;
}
void Ch394::deinit() {
    if (status_.state == State::Disabled) return;
    // poll/receive/send/deinit share a task, so there can be no outstanding SPI
    // transaction when resetting the pins. No EXTI8 subscription is installed.
    reset.write(false); cs.write(true);
    cs.config(GPIO_MODE_INPUT, GPIO_NOPULL);
    reset.config(GPIO_MODE_INPUT, GPIO_NOPULL);
    interrupt_pin.config(GPIO_MODE_INPUT, GPIO_NOPULL);
    status_.state = State::Disabled;
    status_.connected = status_.link = false;
    reset_queues();
}
void Ch394::reset_queues() {
    receive_queue_.clear(); transmit_queue_.clear(); send_pending_ = false; pending_receive_ = 0;
}
bool Ch394::transfer(uint8_t block, uint16_t address, bool write, uint8_t* bytes, size_t size) {
    if (!size || size > 256 || !bytes) return false;
    write_be16(tx_, address);
    tx_[2] = static_cast<uint8_t>(block | (write ? 0x04u : 0u));
    if (write) std::memcpy(tx_ + 3, bytes, size);
    else std::memset(tx_ + 3, 0xff, size);
    // One arbiter transaction holds CS low across the 16-bit address, control,
    // and payload. At 2.625 MHz it satisfies the <=5 MHz timing exception.
    if (!arbiter_.transfer_priority(spi_config, cs, tx_, rx_, size + 3, 5, 3)) return false;
    if (!write) std::memcpy(bytes, rx_ + 3, size);
    if ((block & 0x18u) == common || (block & 0x18u) == socket0) {
        // DS1 tRI = max(0, 0.3*N - 0.3) us for IPv4 register accesses.
        const uint32_t cycles = static_cast<uint32_t>((size * 3 - 3) * (SystemCoreClock / 1000000u) / 10u + 1u);
        const uint32_t start = DWT->CYCCNT;
        unsigned limit = 4096;
        while (static_cast<uint32_t>(DWT->CYCCNT - start) < cycles) {
            if (!limit) return false;
            --limit; __NOP();
        }
    }
    return true;
}
bool Ch394::read8(uint8_t block, uint16_t address, uint8_t& value) {
    return transfer(block, address, false, &value, 1);
}
bool Ch394::write8(uint8_t block, uint16_t address, uint8_t value) {
    return transfer(block, address, true, &value, 1);
}
bool Ch394::read16(uint16_t address, uint16_t& value) {
    uint8_t bytes[2];
    if (!transfer(socket0, address, false, bytes, 2)) return false;
    value = read_be16(bytes); return true;
}
bool Ch394::read16_stable(uint16_t address, uint16_t& value) {
    uint16_t previous, next;
    if (!read16(address, previous)) return false;
    // RX size and TX free can change between byte reads. Never spin forever if
    // a producer keeps updating them; defer this pass after three comparisons.
    for (unsigned i = 0; i < 3; ++i) {
        if (!read16(address, next)) return false;
        if (next == previous) { value = next; return true; }
        previous = next;
    }
    value = 0; // successful transport, unstable count: retry next poll
    return true;
}
bool Ch394::write16(uint16_t address, uint16_t value) {
    uint8_t bytes[2]; write_be16(bytes, value);
    return transfer(socket0, address, true, bytes, 2);
}
bool Ch394::command(uint8_t value, State next, uint32_t now_ms) {
    if (!write8(socket0, sn_ctrl, value)) return false;
    command_next_ = next; status_.state = State::WaitCommand; state_since_ms_ = now_ms;
    return true;
}
void Ch394::fail(Error error, uint32_t now_ms) {
    status_.last_error = error; ++status_.errors;
    status_.connected = status_.link = false;
    status_.state = State::Backoff; state_since_ms_ = now_ms;
    reset_queues(); reset.write(false);
}

bool Ch394::configure_step() {
    // Disable unused sockets before enlarging socket0: no temporary allocation
    // may exceed the independent 16 KiB TX and 16 KiB RX limits.
    if (configure_index_ < 14) {
        const uint8_t socket = static_cast<uint8_t>(1 + configure_index_ / 2);
        const uint16_t reg = configure_index_ & 1 ? sn_tx_size : sn_rx_size;
        if (!write8(static_cast<uint8_t>((socket << 5) | socket0), reg, 0)) return false;
    } else switch (configure_index_) {
        case 14: if (!write8(socket0, sn_rx_size, 4)) return false; break;
        case 15: if (!write8(socket0, sn_tx_size, 4)) return false; break;
        case 16: if (!transfer(common, gwip, true, config_.gateway, 4)) return false; break;
        case 17: if (!transfer(common, smip, true, config_.netmask, 4)) return false; break;
        case 18: if (!transfer(common, source_ip, true, config_.ip, 4)) return false; break;
        case 19: if (!write8(common, chip_config, 0)) return false; break;
        case 20: if (!write8(common, 0x0018, 1)) return false; break; // Socket0 INT, task-polled PC8
        case 21: if (!write8(socket0, sn_mode, 1)) return false; break;
        case 22: if (!write16(sn_port, config_.port)) return false; break;
        case 23:
            if (!transfer(common, source_ip, false, scratch_, 4) || std::memcmp(scratch_, config_.ip, 4)) return false;
            break;
        case 24:
            if (!transfer(common, smip, false, scratch_, 4) || std::memcmp(scratch_, config_.netmask, 4)) return false;
            break;
        case 25:
            if (!transfer(common, gwip, false, scratch_, 4) || std::memcmp(scratch_, config_.gateway, 4)) return false;
            break;
        case 26: if (!read8(common, chip_config, scratch_[0]) || scratch_[0] != 0) return false; break;
        case 27: if (!read8(socket0, sn_rx_size, scratch_[0]) || scratch_[0] != 4) return false; break;
        case 28: if (!read8(socket0, sn_tx_size, scratch_[0]) || scratch_[0] != 4) return false; break;
        default: status_.state = State::OpenSocket; return true;
    }
    ++configure_index_;
    return true;
}
bool Ch394::receive_chunk(uint32_t now_ms) {
    if (!receive_queue_.free()) return true;
    uint16_t count, pointer;
    if (!read16_stable(sn_rx_count, count)) return false;
    if (count > 4096) { fail(Error::BadCount, now_ms); return true; }
    if (!count) return true;
    size_t length = count;
    if (length > 256) length = 256;
    if (length > receive_queue_.free()) length = receive_queue_.free();
    if (!read16(sn_rx_rd, pointer)) return false;
    length = ring_chunk(pointer, length);
    if (!transfer(rx_buffer, pointer & 0x0fffu, false, scratch_, length)
        || !write16(sn_rx_rd, static_cast<uint16_t>(pointer + length))
        || !command(0x40, State::Monitor, now_ms)) return false;
    // Bytes are delivered only after the command register subsequently clears.
    // Hold them in scratch_ while WaitCommand; register transfers only touch
    // tx_/rx_, so the application bytes survive until the acknowledgement.
    pending_receive_ = static_cast<uint16_t>(length);
    return true;
}
bool Ch394::transmit_chunk(uint32_t now_ms) {
    if (send_pending_ || !transmit_queue_.size()) return true;
    uint16_t count, pointer;
    if (!read16_stable(sn_tx_free, count)) return false;
    if (count > 4096) { fail(Error::BadCount, now_ms); return true; }
    if (!count) return true;
    size_t length = transmit_queue_.size();
    if (length > 256) length = 256;
    if (length > count) length = count;
    if (!read16(sn_tx_wr, pointer)) return false;
    length = ring_chunk(pointer, length);
    transmit_queue_.peek(scratch_, length);
    if (!transfer(tx_buffer, pointer & 0x0fffu, true, scratch_, length)
        || !write16(sn_tx_wr, static_cast<uint16_t>(pointer + length))
        || !command(0x20, State::Monitor, now_ms)) return false;
    transmit_queue_.discard(length);
    status_.tx_bytes += length;
    send_pending_ = true; send_since_ms_ = now_ms;
    return true;
}

void Ch394::poll(uint32_t now_ms) {
    uint8_t value;
    switch (status_.state) {
        case State::Disabled: return;
        case State::ResetLow:
            if (static_cast<uint32_t>(now_ms - state_since_ms_) < 1) return;
            reset.write(true); status_.state = State::ResetWait; state_since_ms_ = now_ms; return;
        case State::ResetWait:
            if (static_cast<uint32_t>(now_ms - state_since_ms_) < 40) return;
            status_.state = State::Identify; return;
        case State::Identify:
            // DS1 gives CHIPV reset value 0xXX, not an immutable part ID.
            // This is a bus sanity/version diagnostic, not proof of part identity.
            if (!read8(common, chipv, value) || !read8(common, chipv, status_.chip_version)) {
                fail(Error::Spi, now_ms); return;
            }
            if (value != status_.chip_version || value == 0 || value == 0xff) {
                fail(Error::RegisterCheck, now_ms); return;
            }
            configure_index_ = 0; status_.state = State::Configure; return;
        case State::Configure:
            if (!configure_step()) fail(Error::RegisterCheck, now_ms);
            return;
        case State::OpenSocket:
            reset_queues();
            if (!write8(socket0, sn_int, 0x1f) || !command(1, State::WaitOpen, now_ms)) fail(Error::Spi, now_ms);
            return;
        case State::WaitOpen:
            if (!read8(socket0, sn_sta, value)) { fail(Error::Spi, now_ms); return; }
            if (value == 0x13) {
                if (!command(2, State::Monitor, now_ms)) fail(Error::Spi, now_ms);
            } else if (static_cast<uint32_t>(now_ms - state_since_ms_) >= 100) fail(Error::CommandTimeout, now_ms);
            return;
        case State::WaitCommand:
            if (!read8(socket0, sn_ctrl, value)) { fail(Error::Spi, now_ms); return; }
            if (!value) {
                if (pending_receive_) {
                    receive_queue_.push(scratch_, pending_receive_);
                    status_.rx_bytes += pending_receive_;
                    pending_receive_ = 0;
                }
                status_.state = command_next_; state_since_ms_ = now_ms;
            }
            else if (static_cast<uint32_t>(now_ms - state_since_ms_) >= 100) fail(Error::CommandTimeout, now_ms);
            return;
        case State::Backoff:
            if (static_cast<uint32_t>(now_ms - state_since_ms_) < 250) return;
            ++status_.reconnects; status_.state = State::ResetLow; state_since_ms_ = now_ms; return;
        case State::Monitor: break;
    }
    // A monitor pass does only one phase; no wait loops and <=8 bounded SPI
    // transactions in a payload phase. Motor/control tasks can always preempt.
    switch (monitor_phase_++ % 4) {
        case 0:
            if (!read8(common, phy_cfg, value)) { fail(Error::Spi, now_ms); return; }
            status_.link = value & 1;
            if (!read8(common, gint, value)) { fail(Error::Spi, now_ms); return; }
            if (value & 0x80) { fail(Error::IpConflict, now_ms); return; }
            if (!read8(socket0, sn_sta, status_.socket_state)) { fail(Error::Spi, now_ms); return; }
            status_.connected = status_.link && status_.socket_state == 0x17;
            if (status_.socket_state == 0x00) { status_.state = State::OpenSocket; reset_queues(); }
            else if (status_.socket_state == 0x1c || status_.socket_state == 0x10
                || (!status_.link && status_.socket_state == 0x17)) {
                reset_queues();
                if (!command(0x10, State::OpenSocket, now_ms)) fail(Error::Spi, now_ms);
            }
            return;
        case 1:
            if (!read8(socket0, sn_int, value)) { fail(Error::Spi, now_ms); return; }
            if (value & 0x08) { fail(Error::SocketTimeout, now_ms); return; }
            if (value & 0x10) send_pending_ = false;
            if (value && !write8(socket0, sn_int, value)) { fail(Error::Spi, now_ms); return; }
            if (send_pending_ && static_cast<uint32_t>(now_ms - send_since_ms_) >= 2000) fail(Error::SocketTimeout, now_ms);
            return;
        case 2:
            if (status_.connected && !receive_chunk(now_ms)) fail(Error::Spi, now_ms);
            return;
        case 3:
            if (status_.connected && !transmit_chunk(now_ms)) fail(Error::Spi, now_ms);
            return;
    }
}
size_t Ch394::receive(uint8_t* data, size_t capacity) {
    return receive_queue_.pop(data, capacity);
}
size_t Ch394::send(const uint8_t* data, size_t length) {
    return status_.connected ? transmit_queue_.push(data, length) : 0;
}
} }
