#include "Rehab/platform/phase_adc_monitor.hpp"
#include <cassert>
#include <cstdio>

using rehab::platform::PhaseAdcMonitor;
int main() {
    static_assert(sizeof(PhaseAdcMonitor)==4*sizeof(uint32_t), "bounded ISR state");
    PhaseAdcMonitor monitor;
    assert(monitor.word(0, 123)==0 && monitor.word(1, 123)==0);
    assert(monitor.word(2, 123)==UINT32_MAX && monitor.word(3, 123)==0);
    assert(monitor.word(4, 123)==0);

    // Actual rail-valued ADC results remain raw data; converted validity is separate.
    monitor.record(0, 4095, true, true, false, false, 100);
    assert(monitor.word(0, 101)==1);
    assert(monitor.word(1, 101)==0x83fff000u);
    assert(monitor.word(2, 101)==1 && monitor.word(3, 101)==0);
    monitor.record(4095, 0, true, true, false, false, 101);
    assert(monitor.word(1, 101)==0x83000fffu);

    monitor.record(1234, 2345, true, true, true, true, 102);
    const uint32_t raw=1234u | (2345u << 12);
    assert(monitor.word(1, 102)==(0x8f000000u | raw));
    assert(monitor.word(0, 102)==3);
    // Missing gate readiness suppresses conversion_valid even if incorrectly supplied.
    monitor.record(1234, 2345, true, false, true, false, 103);
    assert(monitor.word(1, 103)==(0x81000000u | raw));
    monitor.record(1234, 2345, true, true, false, true, 104);
    assert(monitor.word(1, 104)==(0x8b000000u | raw));

    // A failed acquisition replaces all stale raw/valid/PWM bits with attempted only.
    monitor.record(1234, 2345, false, true, true, true, 105);
    assert(monitor.word(1, 105)==0x80000000u);
    assert(monitor.word(0, 105)==6 && monitor.word(3, 105)==1);
    assert(monitor.word(2, 112)==7);
    monitor.record(4096, 1, true, true, true, true, 106);
    assert(monitor.word(1, 106)==0x80000000u && monitor.word(3, 106)==2);
    monitor.record(1, UINT32_MAX, true, true, true, true, 107);
    assert(monitor.word(1, 107)==0x80000000u && monitor.word(3, 107)==3);

    // Recovery publishes a new sample without erasing acquisition-error history.
    monitor.record(2048, 2049, true, true, true, false, 0xfffffff0u);
    assert(monitor.word(0, 0x10)==9 && monitor.word(3, 0x10)==3);
    assert(monitor.word(1, 0x10)==(0x87000000u | 2048u | (2049u << 12)));
    assert(monitor.word(2, 0x10)==32);
    assert(monitor.word(2, 0x410)==1056); // Stalled writer remains visibly stale.
    std::puts("PASS phase ADC observer: raw/valid separation, failure, age/wrap and recovery");
}
