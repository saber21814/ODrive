#pragma once
#include "Rehab/core/engine.hpp"

namespace rehab { namespace drivers {
class Hx711 {
public:
    bool init(bool rate80 = true);
    void deinit();
    void poll(uint32_t now_us);
    bool enabled() const { return enabled_; }
    const ForceSample& sample() const { return sample_; }
    uint32_t timing_faults() const { return timing_faults_; }
private:
    bool pulse(bool& bit);
    ForceSample sample_{};
    uint32_t settling_started_us_ = 0, settling_us_ = 50000;
    uint32_t last_read_us_ = 0, conversion_us_ = 12500;
    uint32_t timing_faults_ = 0;
    bool enabled_ = false, settling_started_ = false, have_read_ = false;
};
} }
