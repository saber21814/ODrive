#ifndef AS5600_UTILS_HPP
#define AS5600_UTILS_HPP

#include <stdint.h>

namespace as5600 {

static constexpr int32_t CPR = 4096;
static constexpr uint32_t STALE_TICKS = 16; // 2 ms at the 8 kHz control rate
static constexpr uint32_t STATUS_STALE_TICKS = 80; // 10 ms at 8 kHz
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

inline bool transaction_is_status(uint8_t slot) {
    return (slot & 7u) == 7u;
}

inline bool transaction_can_start(bool pending, bool recovery_requested,
                                  bool recovery_blocked, bool hal_ready,
                                  bool bus_busy) {
    return !pending && !recovery_requested && !recovery_blocked &&
           hal_ready && !bus_busy;
}

inline bool recovery_can_run(bool requested, bool axis_present, bool armed) {
    return requested && axis_present && !armed;
}

inline uint8_t update_failure_count(uint8_t current, bool success) {
    if (success)
        return 0;
    return current == UINT8_MAX ? UINT8_MAX : (uint8_t)(current + 1u);
}

inline bool status_is_stale(uint32_t now_tick, uint32_t status_tick) {
    return (uint32_t)(now_tick - status_tick) > STATUS_STALE_TICKS;
}

inline int32_t resume_multiturn(int32_t shadow, uint16_t raw, uint16_t old_raw) {
    return shadow + wrapped_delta(raw, old_raw);
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
