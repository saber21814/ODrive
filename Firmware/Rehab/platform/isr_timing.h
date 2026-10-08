#ifndef REHAB_PLATFORM_ISR_TIMING_H
#define REHAB_PLATFORM_ISR_TIMING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable diagnostic order: maximum cycles [0..4], invocation counts [0..4]. */
typedef enum RehabIsrChannel {
    REHAB_ISR_CONTROL_LOOP = 0,
    REHAB_ISR_TIM8_UPDATE = 1,
    REHAB_ISR_SPI_DMA_TX = 2,
    REHAB_ISR_SPI_DMA_RX = 3,
    REHAB_ISR_EXTI9_5 = 4,
    REHAB_ISR_CHANNEL_COUNT = 5
} RehabIsrChannel;

/* DWT must already be enabled by V1.2 early_init(). Each channel has exactly
 * one IRQ writer. Measurements include time spent preempted by other IRQs.
 * Unsigned cycle subtraction tolerates one CYCCNT wrap; a measured interval
 * must be shorter than 2^32 cycles. Invocation counters wrap modulo 2^32.
 * Instrumentation covers the handler body, not exception entry/exit or the
 * final counter-update overhead. This is telemetry, not a verified HW WCET.
 */
uint32_t rehab_isr_begin(void);
void rehab_isr_end(unsigned channel, uint32_t start);
/* Each getter makes a short PRIMASK-protected read and restores the old mask.
 * Invalid channels return zero. No reset API races with the IRQ writers.
 */
uint32_t rehab_isr_max_cycles(unsigned channel);
uint32_t rehab_isr_count(unsigned channel);

#ifdef __cplusplus
} // extern "C"

namespace rehab { namespace platform {
/* Put this object first in a C++ handler: every ordinary return is measured. */
class ScopedIsrTiming {
public:
    explicit ScopedIsrTiming(unsigned channel) noexcept
        : channel_(channel), start_(rehab_isr_begin()) {}
    ~ScopedIsrTiming() noexcept { rehab_isr_end(channel_, start_); }
    ScopedIsrTiming(const ScopedIsrTiming&) = delete;
    ScopedIsrTiming& operator=(const ScopedIsrTiming&) = delete;
private:
    unsigned channel_;
    uint32_t start_;
};
} }
#endif

#endif
