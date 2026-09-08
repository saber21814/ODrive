#ifndef AS5600_UTILS_HPP
#define AS5600_UTILS_HPP

#include <stdint.h>

namespace as5600 {

static constexpr int32_t CPR = 4096;
static constexpr uint32_t STALE_TICKS = 16; // 2 ms at the 8 kHz control rate
static constexpr uint32_t STATUS_STALE_TICKS = 80; // 10 ms at 8 kHz
static constexpr uint8_t MAX_CONSECUTIVE_FAILURES = 3;
static constexpr uint8_t MAX_CONSECUTIVE_REJECTED_SAMPLES = 3;
static constexpr uint8_t RECOVERY_CLOCKS = 9;
static constexpr uint8_t MAX_RECOVERY_ATTEMPTS = 5;
static constexpr uint32_t RECOVERY_BASE_COOLDOWN_TICKS = 800; // 100 ms at 8 kHz
static constexpr uint32_t RECOVERY_MAX_COOLDOWN_TICKS = 8000; // 1 s at 8 kHz
static constexpr float PLAUSIBILITY_SPEED_MARGIN = 1.25f;
static constexpr uint32_t PLAUSIBILITY_COUNT_MARGIN = 4;
static constexpr uint32_t PLAUSIBILITY_ABSOLUTE_MAX_DELTA = 192;

struct SamplePlausibility {
    int32_t raw_delta;
    uint32_t max_abs_raw_delta;
    bool accepted;
};

struct RecoveryState {
    bool requested;
    bool blocked;
    uint8_t attempts;
    uint32_t next_tick;
};

inline uint16_t decode_raw_angle(uint8_t msb, uint8_t lsb) {
    return (uint16_t)(((uint16_t)(msb & 0x0f) << 8) | lsb);
}

inline int32_t wrapped_delta(uint16_t newer, uint16_t older) {
    int32_t delta = (int32_t)newer - (int32_t)older;
    if (delta > CPR / 2) delta -= CPR;
    if (delta < -CPR / 2) delta += CPR;
    return delta;
}

inline uint32_t max_plausible_raw_delta(uint32_t elapsed_ticks,
                                        float tick_period,
                                        float max_mechanical_speed) {
    // A 25% speed allowance plus four counts covers transaction jitter and
    // AS5600 quantization. The absolute cap still accepts 1000 rpm after a
    // near-stale 2 ms gap, but never admits a hundreds/thousands-count jump.
    float limit = max_mechanical_speed * (float)CPR *
                  (float)elapsed_ticks * tick_period *
                  PLAUSIBILITY_SPEED_MARGIN;
    if (limit >= (float)(PLAUSIBILITY_ABSOLUTE_MAX_DELTA -
                         PLAUSIBILITY_COUNT_MARGIN))
        return PLAUSIBILITY_ABSOLUTE_MAX_DELTA;
    uint32_t rounded_up = limit > 0.0f ? (uint32_t)limit + 1u : 0u;
    uint32_t with_margin = rounded_up + PLAUSIBILITY_COUNT_MARGIN;
    return with_margin < PLAUSIBILITY_ABSOLUTE_MAX_DELTA ?
        with_margin : PLAUSIBILITY_ABSOLUTE_MAX_DELTA;
}

inline SamplePlausibility check_sample(uint16_t raw, uint16_t last_raw,
                                       uint32_t sample_tick,
                                       uint32_t last_sample_tick,
                                       float tick_period,
                                       float max_mechanical_speed) {
    SamplePlausibility result = {};
    result.raw_delta = wrapped_delta(raw, last_raw);
    result.max_abs_raw_delta = max_plausible_raw_delta(
        (uint32_t)(sample_tick - last_sample_tick), tick_period,
        max_mechanical_speed);
    const uint32_t magnitude = result.raw_delta < 0 ?
        (uint32_t)(-result.raw_delta) : (uint32_t)result.raw_delta;
    result.accepted = magnitude <= result.max_abs_raw_delta;
    return result;
}

inline float pll_feedback_period(bool accepted_sample,
                                 uint32_t elapsed_ticks,
                                 float tick_period) {
    if (!accepted_sample)
        return 0.0f;
    const uint32_t bounded_ticks = elapsed_ticks < STALE_TICKS ?
        elapsed_ticks : STALE_TICKS;
    return (float)bounded_ticks * tick_period;
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

inline bool tick_reached(uint32_t now_tick, uint32_t deadline_tick) {
    return (int32_t)(now_tick - deadline_tick) >= 0;
}

inline uint32_t recovery_cooldown_ticks(uint8_t attempts) {
    if (attempts == 0)
        return 0;
    uint32_t cooldown = RECOVERY_BASE_COOLDOWN_TICKS;
    for (uint8_t i = 1; i < attempts && cooldown < RECOVERY_MAX_COOLDOWN_TICKS; ++i) {
        cooldown <<= 1;
        if (cooldown > RECOVERY_MAX_COOLDOWN_TICKS)
            cooldown = RECOVERY_MAX_COOLDOWN_TICKS;
    }
    return cooldown;
}

inline RecoveryState request_recovery(RecoveryState state, uint32_t now_tick) {
    if (!state.requested) {
        state.attempts = 0;
        state.next_tick = now_tick;
    }
    state.requested = true;
    state.blocked = true;
    return state;
}

inline RecoveryState recovery_after_attempt(RecoveryState state, bool success,
                                            uint32_t now_tick) {
    if (success)
        return {false, false, 0, now_tick};

    state.requested = true;
    state.blocked = true;
    if (state.attempts < MAX_RECOVERY_ATTEMPTS)
        ++state.attempts;
    state.next_tick = now_tick + recovery_cooldown_ticks(state.attempts);
    return state;
}

inline RecoveryState rearm_recovery_after_clear(RecoveryState state,
                                                bool errors_cleared,
                                                uint32_t now_tick) {
    if (state.requested && state.blocked &&
        state.attempts >= MAX_RECOVERY_ATTEMPTS && errors_cleared) {
        state.attempts = 0;
        state.next_tick = now_tick;
    }
    return state;
}

inline bool recovery_attempt_due(const RecoveryState& state,
                                 uint32_t now_tick) {
    return state.requested && state.blocked &&
           state.attempts < MAX_RECOVERY_ATTEMPTS &&
           tick_reached(now_tick, state.next_tick);
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

inline bool sample_rejection_failed(uint8_t consecutive_rejections) {
    return consecutive_rejections >= MAX_CONSECUTIVE_REJECTED_SAMPLES;
}

inline bool recovery_clocks_complete(uint8_t completed_clocks) {
    return completed_clocks >= RECOVERY_CLOCKS;
}

} // namespace as5600

#endif
