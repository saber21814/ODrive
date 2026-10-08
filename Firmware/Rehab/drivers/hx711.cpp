#include "hx711.hpp"
#include "driver_helpers.hpp"
#include "stm32_gpio.hpp"
#include "stm32_system.h"

namespace rehab { namespace drivers {
namespace {
Stm32Gpio dout{GPIOC, GPIO_PIN_6};
Stm32Gpio sck{GPIOC, GPIO_PIN_7};
Stm32Gpio rate{GPIOB, GPIO_PIN_1};
bool low_delay() {
    const uint32_t start = DWT->CYCCNT;
    const uint32_t cycles = SystemCoreClock / 2000000u;
    unsigned budget = 64;
    while (static_cast<uint32_t>(DWT->CYCCNT - start) < cycles) {
        if (!budget) return false;
        --budget; __NOP();
    }
    return true;
}
}
bool Hx711::init(bool rate80) {
    if (enabled_) deinit();
    sample_ = ForceSample{};
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const uint32_t before = DWT->CYCCNT;
    for (unsigned i = 0; i < 16; ++i) __NOP();
    if (DWT->CYCCNT == before || SystemCoreClock < 8000000u) return false;
    sck.write(false);
    rate.write(rate80);
    if (!dout.config(GPIO_MODE_INPUT, GPIO_PULLUP)
        || !sck.config(GPIO_MODE_OUTPUT_PP, GPIO_NOPULL)
        || !rate.config(GPIO_MODE_OUTPUT_PP, GPIO_NOPULL)) {
        deinit(); return false;
    }
    settling_us_ = rate80 ? 50000 : 400000;
    conversion_us_ = rate80 ? 12500 : 100000;
    settling_started_ = false;
    have_read_ = false;
    enabled_ = true;
    return true;
}
void Hx711::deinit() {
    sck.write(false);
    sck.config(GPIO_MODE_INPUT, GPIO_NOPULL);
    dout.config(GPIO_MODE_INPUT, GPIO_NOPULL);
    rate.config(GPIO_MODE_INPUT, GPIO_NOPULL);
    enabled_ = false; sample_.valid = false;
}
bool Hx711::pulse(bool& bit) {
    uint32_t elapsed;
    bool timed = true;
    {
        // Only the high pulse is masked: nominal 0.75 us, measured maximum 2 us.
        // Keeping low intervals preemptible avoids starving motor ADC interrupts.
        CriticalSectionContext lock;
        const uint32_t start = DWT->CYCCNT;
        GPIOC->BSRR = GPIO_PIN_7;
        // One shared fallback budget bounds both waits even if DWT stalls.
        // Exhaustion always drives SCK low and invalidates the conversion.
        unsigned budget = 32;
        while (static_cast<uint32_t>(DWT->CYCCNT - start) < SystemCoreClock / 2000000u) {
            if (!budget) { timed = false; break; }
            --budget; __NOP();
        }
        bit = (GPIOC->IDR & GPIO_PIN_6) != 0;
        while (timed && static_cast<uint32_t>(DWT->CYCCNT - start) < (SystemCoreClock / 4000000u) * 3u) {
            if (!budget) { timed = false; break; }
            --budget; __NOP();
        }
        GPIOC->BSRR = static_cast<uint32_t>(GPIO_PIN_7) << 16;
        elapsed = DWT->CYCCNT - start;
    }
    const bool low_ok = low_delay();
    return timed && low_ok && elapsed <= SystemCoreClock / 500000u;
}
void Hx711::poll(uint32_t now_us) {
    if (!enabled_) return;
    if (!settling_started_) { settling_started_ = true; settling_started_us_ = now_us; }
    if (dout.read()) return; // Never wait for DOUT; stale age is checked by core.
    // DOUT glitches cannot turn a 10/80 SPS converter into a 1 kHz source. Use
    // actual conversion acquisition timestamps, never synthesize intermediate samples.
    if (have_read_ && static_cast<uint32_t>(now_us - last_read_us_) < conversion_us_) return;
    have_read_ = true; last_read_us_ = now_us;
    uint32_t bits = 0;
    bool timing_ok = true;
    for (unsigned i = 0; i < 25; ++i) {
        bool bit;
        if (!pulse(bit)) timing_ok = false;
        if (i < 24) bits = (bits << 1) | (bit ? 1u : 0u);
    }
    if (!timing_ok) {
        ++timing_faults_;
        sample_.valid = false;
        settling_started_us_ = now_us;
        return;
    }
    // HX711 raises DOUT after the 25th PD_SCK pulse. A stuck-low wire is not
    // a ready stream of zero-force conversions and must not refresh validity.
    // pulse() already supplied a >=0.5 us low interval before this check.
    if (!dout.read()) { sample_.valid = false; return; }
    if (static_cast<uint32_t>(now_us - settling_started_us_) < settling_us_) return;
    sample_.raw = sign_extend_24(bits);
    sample_.timestamp_us = now_us;
    ++sample_.sequence;
    sample_.valid = true;
}
} }
