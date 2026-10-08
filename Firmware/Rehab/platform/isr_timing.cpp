#include "isr_timing.h"
#include <stm32_system.h>

namespace {
alignas(4) uint32_t maxima[REHAB_ISR_CHANNEL_COUNT]{};
alignas(4) uint32_t counts[REHAB_ISR_CHANNEL_COUNT]{};
}

extern "C" uint32_t rehab_isr_begin(void) {
    return DWT->CYCCNT;
}

extern "C" void rehab_isr_end(unsigned channel, uint32_t start) {
    const uint32_t elapsed = static_cast<uint32_t>(DWT->CYCCNT - start);
    if (channel >= REHAB_ISR_CHANNEL_COUNT) return;
    // One non-reentrant IRQ owns each element. Plain atomic loads/stores avoid
    // read/modify/write retry loops, waiting locks, and interrupt masking here.
    const uint32_t previous_max = __atomic_load_n(&maxima[channel], __ATOMIC_RELAXED);
    if (elapsed > previous_max) __atomic_store_n(&maxima[channel], elapsed, __ATOMIC_RELAXED);
    const uint32_t previous_count = __atomic_load_n(&counts[channel], __ATOMIC_RELAXED);
    __atomic_store_n(&counts[channel], previous_count + 1u, __ATOMIC_RELAXED);
}

extern "C" uint32_t rehab_isr_max_cycles(unsigned channel) {
    if (channel >= REHAB_ISR_CHANNEL_COUNT) return 0;
    CriticalSectionContext snapshot;
    return __atomic_load_n(&maxima[channel], __ATOMIC_RELAXED);
}

extern "C" uint32_t rehab_isr_count(unsigned channel) {
    if (channel >= REHAB_ISR_CHANNEL_COUNT) return 0;
    CriticalSectionContext snapshot;
    return __atomic_load_n(&counts[channel], __ATOMIC_RELAXED);
}
