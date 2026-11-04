#pragma once
#include "Rehab/core/engine.hpp"
namespace rehab { namespace drivers {
// Runtime integration carrier only; actual HX711 timing is tested separately.
class Hx711 {
public:
    bool init(bool=true) { return true; }
    void poll(uint32_t us) { sample_.timestamp_us=us; ++sample_.sequence; }
    const ForceSample& sample() const { return sample_; }
    uint32_t timing_faults() const { return 0; }
private:
    ForceSample sample_{};
};
}}
