#pragma once
#include <cstdint>

namespace rehab { namespace platform {
// One ControlLoop ISR writer. Readers must mask interrupts while calling word().
// Values describe the DC-calibration acquisition, not reconstructed phase current.
class PhaseAdcMonitor {
public:
    void record(uint32_t b, uint32_t c, bool complete, bool gate,
                bool conversion_valid, bool pwm, uint32_t now_ms) {
        ++sequence_;
        sample_ms_ = now_ms;
        if (!complete || b > 4095u || c > 4095u) {
            packed_ = 0x80000000u;
            ++errors_;
            return;
        }
        packed_ = 0x81000000u | b | (c << 12)
            | (gate ? (1u << 25) : 0u)
            | (gate && conversion_valid ? (1u << 26) : 0u)
            | (pwm ? (1u << 27) : 0u);
    }
    uint32_t word(unsigned index, uint32_t now_ms) const {
        switch (index) {
            case 0: return sequence_;
            case 1: return packed_;
            case 2: return (packed_ & 0x80000000u) ? now_ms - sample_ms_ : UINT32_MAX;
            case 3: return errors_;
            default: return 0;
        }
    }
private:
    volatile uint32_t sequence_ = 0;
    volatile uint32_t packed_ = 0;
    volatile uint32_t sample_ms_ = 0;
    volatile uint32_t errors_ = 0;
};
}}
