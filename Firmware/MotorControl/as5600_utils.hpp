#ifndef AS5600_UTILS_HPP
#define AS5600_UTILS_HPP

#include <stdint.h>

namespace as5600 {

static constexpr int32_t CPR = 4096;
static constexpr uint32_t STALE_TICKS = 16; // 2 ms at the 8 kHz control rate
static constexpr uint8_t MAX_CONSECUTIVE_FAILURES = 3;
static constexpr uint8_t RECOVERY_CLOCKS = 9;

inline uint16_t decode_raw_angle(uint8_t msb, uint8_t lsb) {
    return (uint16_t)(((uint16_t)(msb & 0x0f) << 8) | lsb);
}

inline int32_t wrapped_delta(uint16_t newer, uint16_t older) {
    int32_t delta = (int32_t)newer - (int32_t)older;
    if (delta > CPR / 2) delta -= CPR;
    if (delta < -CPR / 2) delta += CPR;
    return delta;
}

inline uint16_t configure_conf(uint16_t conf) {
    // Preserve HYST, OUTS and PWMF. Only volatile runtime controls are changed.
    constexpr uint16_t clear_mask = (uint16_t)((3u << 0) | (3u << 8) |
                                                (7u << 10) | (1u << 13));
    constexpr uint16_t set_mask = (uint16_t)((3u << 8) | (1u << 10));
    return (uint16_t)((conf & ~clear_mask) | set_mask);
}

inline bool magnet_detected(uint8_t status) { return (status & 0x20u) != 0; }
inline bool magnet_too_weak(uint8_t status) { return (status & 0x10u) != 0; }
inline bool magnet_too_strong(uint8_t status) { return (status & 0x08u) != 0; }

inline bool sample_is_stale(uint32_t now_tick, uint32_t sample_tick) {
    return (uint32_t)(now_tick - sample_tick) > STALE_TICKS;
}

inline bool communication_failed(uint8_t consecutive_failures) {
    return consecutive_failures >= MAX_CONSECUTIVE_FAILURES;
}

inline bool recovery_clocks_complete(uint8_t completed_clocks) {
    return completed_clocks >= RECOVERY_CLOCKS;
}

} // namespace as5600

#endif
