// Compile the real timing implementation with only DWT/PRIMASK substituted.
// This tests counter semantics, not actual MCU interrupt execution time.
#include "Rehab/platform/isr_timing.h"
#include <stm32_system.h>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

TimingDwt timing_dwt;
uint32_t timing_primask = 0;
unsigned timing_critical_entries = 0, timing_critical_depth = 0;
static unsigned checks = 0;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "FAIL isr_timing_tests.cpp:%d: %s\n", __LINE__, #condition); \
    std::exit(1); } } while (0)

static void c_style_irq(uint32_t cycles) {
    const uint32_t start = rehab_isr_begin();
    timing_dwt.CYCCNT += cycles;
    rehab_isr_end(REHAB_ISR_SPI_DMA_TX, start);
}
static void cpp_style_irq(bool early_return, uint32_t cycles) {
    rehab::platform::ScopedIsrTiming timing(REHAB_ISR_CONTROL_LOOP);
    timing_dwt.CYCCNT += cycles;
    if (early_return) return;
    timing_dwt.CYCCNT += 5;
}

int main() {
    static_assert(REHAB_ISR_CONTROL_LOOP == 0 && REHAB_ISR_TIM8_UPDATE == 1 &&
        REHAB_ISR_SPI_DMA_TX == 2 && REHAB_ISR_SPI_DMA_RX == 3 &&
        REHAB_ISR_EXTI9_5 == 4 && REHAB_ISR_SPI1_DMA_TX == 5 &&
        REHAB_ISR_SPI1_DMA_RX == 6 && REHAB_ISR_CHANNEL_COUNT == 7,
        "diagnostic channel order is a stable interface");
    static_assert(!std::is_copy_constructible<rehab::platform::ScopedIsrTiming>::value,
        "copying a scope would double-count an interrupt");
    for (unsigned channel = 0; channel < REHAB_ISR_CHANNEL_COUNT; ++channel) {
        CHECK(rehab_isr_max_cycles(channel) == 0);
        CHECK(rehab_isr_count(channel) == 0);
    }
    CHECK(timing_primask == 0 && timing_critical_depth == 0);

    timing_dwt.CYCCNT = 100;
    const unsigned before = timing_critical_entries;
    c_style_irq(37);
    CHECK(timing_critical_entries == before); // IRQ writer never takes a lock
    CHECK(rehab_isr_max_cycles(REHAB_ISR_SPI_DMA_TX) == 37);
    CHECK(rehab_isr_count(REHAB_ISR_SPI_DMA_TX) == 1);
    c_style_irq(12);
    CHECK(rehab_isr_max_cycles(REHAB_ISR_SPI_DMA_TX) == 37);
    CHECK(rehab_isr_count(REHAB_ISR_SPI_DMA_TX) == 2);
    c_style_irq(71);
    CHECK(rehab_isr_max_cycles(REHAB_ISR_SPI_DMA_TX) == 71);
    CHECK(rehab_isr_count(REHAB_ISR_SPI_DMA_TX) == 3);

    timing_dwt.CYCCNT = 1000;
    cpp_style_irq(false, 20);
    CHECK(rehab_isr_count(REHAB_ISR_CONTROL_LOOP) == 1);
    CHECK(rehab_isr_max_cycles(REHAB_ISR_CONTROL_LOOP) == 25);
    cpp_style_irq(true, 43); // RAII records the return path too
    CHECK(rehab_isr_count(REHAB_ISR_CONTROL_LOOP) == 2);
    CHECK(rehab_isr_max_cycles(REHAB_ISR_CONTROL_LOOP) == 43);

    // Subtraction remains valid across the DWT counter's unsigned wrap.
    timing_dwt.CYCCNT = 0xfffffff0u;
    const uint32_t wrapped_start = rehab_isr_begin();
    timing_dwt.CYCCNT = 0x20u;
    rehab_isr_end(REHAB_ISR_SPI_DMA_RX, wrapped_start);
    CHECK(rehab_isr_max_cycles(REHAB_ISR_SPI_DMA_RX) == 48);
    CHECK(rehab_isr_count(REHAB_ISR_SPI_DMA_RX) == 1);

    // A nested higher-priority IRQ updates its own channel and contributes to
    // the interrupted handler's elapsed wall cycles (preemption is included).
    timing_dwt.CYCCNT = 2000;
    {
        rehab::platform::ScopedIsrTiming outer(REHAB_ISR_EXTI9_5);
        timing_dwt.CYCCNT += 7;
        {
            rehab::platform::ScopedIsrTiming inner(REHAB_ISR_TIM8_UPDATE);
            timing_dwt.CYCCNT += 90;
        }
        timing_dwt.CYCCNT += 3;
    }
    CHECK(rehab_isr_max_cycles(REHAB_ISR_TIM8_UPDATE) == 90);
    CHECK(rehab_isr_count(REHAB_ISR_TIM8_UPDATE) == 1);
    CHECK(rehab_isr_max_cycles(REHAB_ISR_EXTI9_5) == 100);
    CHECK(rehab_isr_count(REHAB_ISR_EXTI9_5) == 1);
    CHECK(rehab_isr_max_cycles(REHAB_ISR_SPI_DMA_TX) == 71);
    CHECK(rehab_isr_max_cycles(REHAB_ISR_CONTROL_LOOP) == 43);

    // A diagnostic read must preserve an already-held PRIMASK, not enable IRQs.
    timing_primask = 1;
    const unsigned reads_before = timing_critical_entries;
    CHECK(rehab_isr_max_cycles(REHAB_ISR_EXTI9_5) == 100);
    CHECK(rehab_isr_count(REHAB_ISR_EXTI9_5) == 1);
    CHECK(timing_critical_entries == reads_before + 2);
    CHECK(timing_primask == 1 && timing_critical_depth == 0);
    timing_primask = 0;
    CHECK(rehab_isr_count(REHAB_ISR_TIM8_UPDATE) == 1 && timing_primask == 0);

    const unsigned invalid_before = timing_critical_entries;
    rehab_isr_end(REHAB_ISR_CHANNEL_COUNT, 0);
    rehab_isr_end(std::numeric_limits<unsigned>::max(), 0);
    CHECK(rehab_isr_count(REHAB_ISR_CHANNEL_COUNT) == 0);
    CHECK(rehab_isr_max_cycles(std::numeric_limits<unsigned>::max()) == 0);
    CHECK(timing_critical_entries == invalid_before);
    CHECK(rehab_isr_count(REHAB_ISR_EXTI9_5) == 1);
    std::printf("ISR timing: %u checks passed (real implementation; mocked DWT/PRIMASK)\n", checks);
}
