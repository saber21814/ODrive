#pragma once
// V1.3 hardware STOP resets DRV8301 registers. Keep that reset policy outside
// the original driver: only its public config/init/readback API is used here.
#include <Drivers/DRV8301/drv8301.hpp>
#include <Drivers/STM32/stm32_system.h>
#include <utils.hpp>

class V13GateDriver : public GateDriverBase, public OpAmpBase {
public:
    using FaultType_e = Drv8301::FaultType_e;
    static constexpr FaultType_e FaultType_NoFault = Drv8301::FaultType_NoFault;
    using Check = bool (*)();

    V13GateDriver(Stm32SpiArbiter* bus, Stm32Gpio cs, Stm32Gpio nfault,
                  Stm32Gpio gate, Check stop_asserted, Check unsafe_to_initialize)
        : driver_(bus, cs, {}, nfault), gate_(gate), stop_(stop_asserted),
          unsafe_(unsafe_to_initialize) {}

    bool config(float requested_gain, float* actual_gain) {
        // This method and init belong to the native axis task. ISR code never
        // modifies the original driver's cached register configuration.
        const bool ok = driver_.config(requested_gain, actual_gain);
        if (ok) { requested_gain_ = requested_gain; configured_ = true; }
        CRITICAL_SECTION() {
            if (!ok || !driver_.is_ready()) { ready_ = false; }
        }
        return ok;
    }

    bool init() {
        if (!configured_) { return false; }
        uint32_t epoch = 0;
        CRITICAL_SECTION() {
            if (stop_() || unsafe_()) { return false; }
            if (ready_ && driver_.is_ready()) { return true; }
            epoch = generation_;
            ready_ = false;
            // A newly observed low must invalidate this attempt even if a fast
            // guard is its first observer (rather than an EXTI edge).
            low_latched_ = false;
            gate_.write(false);
        }
        delay_us(40); // >=20 us full-reset pulse required by DRV8301

        // Changing between two different legal gains always invalidates the
        // original private ready cache. Restore the selected gain before init.
        // No private access, placement-new, or modification of the core driver.
        float ignored_gain;
        const bool configured = driver_.config(10.0f, &ignored_gain)
            && driver_.config(80.0f, &ignored_gain)
            && driver_.config(requested_gain_, &ignored_gain);
        CRITICAL_SECTION() {
            if (!configured || generation_ != epoch || stop_() || unsafe_()) {
                gate_.write(false);
                return false;
            }
            gate_.write(true);
        }
        // Native init writes and reads back both control registers and faults.
        // Interrupts remain enabled during its bounded SPI transactions/waits.
        const bool initialized = driver_.init();
        CRITICAL_SECTION() {
            if (initialized && generation_ == epoch && !stop_() && !unsafe_()
                    && driver_.is_ready()) {
                ready_ = true;
                return true;
            }
            ready_ = false;
            gate_.write(false);
        }
        return false;
    }

    // Called only after motor PWM has been disarmed. This is ISR-safe: no SPI,
    // RTOS calls, sleeps, or mutation of the original driver's state/cache.
    void physical_stop(bool edge) {
        CRITICAL_SECTION() {
            if (edge || !low_latched_) { ++generation_; }
            low_latched_ = true;
            ready_ = false;
            gate_.write(false);
        }
    }
    uint32_t generation() const {
        CRITICAL_SECTION() { return generation_; }
        return 0;
    }
    void do_checks() {
        driver_.do_checks();
        CRITICAL_SECTION() {
            if (!driver_.is_ready()) { ready_ = false; }
        }
    }
    bool is_ready() final {
        CRITICAL_SECTION() {
            return ready_ && !stop_() && driver_.is_ready();
        }
        return false;
    }
    bool set_enabled(bool enabled) final {
        // Native disarm removes PWM. Normal idle must retain opamp bias and
        // must not reset the DRV on every pass through Motor::setup().
        return !enabled || is_ready();
    }
    FaultType_e get_error() { return driver_.get_error(); }
    float get_midpoint() final { return driver_.get_midpoint(); }
    float get_max_output_swing() final { return driver_.get_max_output_swing(); }

private:
    Drv8301 driver_;
    Stm32Gpio gate_;
    Check stop_;
    Check unsafe_;
    float requested_gain_ = 10.0f;
    bool configured_ = false;
    // Every access to these ISR-shared fields is under CRITICAL_SECTION.
    volatile bool ready_ = false;
    volatile bool low_latched_ = false;
    volatile uint32_t generation_ = 0;
};
