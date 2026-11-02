#pragma once
#include <Drivers/gate_driver.hpp>
#include <Drivers/STM32/stm32_gpio.hpp>
#include <vector>
#include <cstdint>
struct Stm32SpiArbiter {};
extern std::vector<float> mock_gain_calls;
extern unsigned mock_inner_init_calls, mock_register_writes;
extern bool mock_init_success;
extern void (*mock_init_hook)();
extern void (*mock_config_hook)();
class Drv8301 : public GateDriverBase, public OpAmpBase {
public:
    enum FaultType_e : uint32_t { FaultType_NoFault = 0 };
    Drv8301(Stm32SpiArbiter*, Stm32Gpio, Stm32Gpio, Stm32Gpio) {}
    bool config(float gain, float* actual) {
        mock_gain_calls.push_back(gain);
        if (gain_ != gain) { ready_ = false; }
        gain_ = gain;
        if (actual) { *actual = gain; }
        if (mock_config_hook) { mock_config_hook(); }
        return true;
    }
    bool init() {
        ++mock_inner_init_calls;
        if (ready_) { return true; }
        ++mock_register_writes;
        ready_ = mock_init_success;
        if (mock_init_hook) { mock_init_hook(); }
        return ready_;
    }
    void do_checks() {}
    bool is_ready() final { return ready_; }
    bool set_enabled(bool) final { return true; }
    FaultType_e get_error() { return FaultType_NoFault; }
    float get_midpoint() final { return 0.5f; }
    float get_max_output_swing() final { return 1.35f / 1.65f; }
private:
    float gain_ = 0;
    bool ready_ = false;
};
