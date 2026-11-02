#include <Rehab/drivers/v13_gate_driver.hpp>
#include <cassert>
#include <cstdio>

unsigned mock_critical_depth = 0;
bool mock_gate = false;
unsigned mock_gate_rises = 0;
std::vector<float> mock_gain_calls;
unsigned mock_inner_init_calls = 0, mock_register_writes = 0;
bool mock_init_success = true;
void (*mock_init_hook)() = nullptr;
void (*mock_config_hook)() = nullptr;
void (*delay_hook)() = nullptr;
bool stop_low = false, unsafe_motor = false;
V13GateDriver* active = nullptr;
bool stop_asserted() { return stop_low; }
bool unsafe_to_init() { return unsafe_motor; }
void delay_us(uint32_t us) {
    assert(!mock_critical_depth);
    assert(us >= 40);
    assert(!mock_gate);
    if (delay_hook) { delay_hook(); }
}
void stop_during_init() {
    stop_low = true;
    active->physical_stop(true);
    // Even a short pulse which has already recovered must invalidate init.
    stop_low = false;
}
void reset() {
    mock_gate = false; mock_gate_rises = 0; mock_gain_calls.clear();
    mock_inner_init_calls = mock_register_writes = 0;
    mock_init_success = true; mock_init_hook = mock_config_hook = delay_hook = nullptr;
    stop_low = unsafe_motor = false;
}
int main() {
    // Startup with STOP low may neither enable GATE nor touch SPI/init.
    reset();
    V13GateDriver startup(nullptr, {}, {}, {}, stop_asserted, unsafe_to_init);
    active = &startup;
    float gain = 0;
    assert(startup.config(40, &gain) && gain == 40);
    stop_low = true;
    startup.physical_stop(false);
    const auto first_epoch = startup.generation();
    startup.physical_stop(false);
    assert(first_epoch == startup.generation());
    assert(!startup.init() && !startup.is_ready());
    assert(!mock_gate && !mock_inner_init_calls);
    stop_low = false;
    assert(startup.init() && startup.is_ready() && mock_gate);
    assert(mock_register_writes == 1);
    assert(mock_gain_calls.size() == 4);
    assert(mock_gain_calls[1] == 10 && mock_gain_calls[2] == 80 && mock_gain_calls[3] == 40);

    // Normal idle/setup repeats retain opamp bias and do not reset registers.
    const auto rises = mock_gate_rises;
    assert(startup.set_enabled(false) && mock_gate);
    assert(startup.config(40, &gain) && startup.init());
    assert(mock_gate_rises == rises && mock_inner_init_calls == 1);
    assert(startup.get_midpoint() == 0.5f);
    assert(startup.get_error() == V13GateDriver::FaultType_NoFault);

    // STOP invalidates wrapper ready while inner driver deliberately retains
    // its old ready cache. Recovery must force native writes/readback again.
    stop_low = true;
    startup.physical_stop(true);
    assert(!startup.is_ready() && !mock_gate && !startup.set_enabled(true));
    const auto stopped_epoch = startup.generation();
    startup.physical_stop(false);
    assert(stopped_epoch == startup.generation());
    stop_low = false;
    assert(!startup.is_ready() && !startup.set_enabled(true));
    assert(startup.init() && startup.is_ready());
    assert(mock_register_writes == 2 && mock_gate_rises == rises + 1);

    // A new STOP during the task delay blocks GATE before it rises.
    startup.physical_stop(true);
    const auto before = mock_gate_rises;
    delay_hook = stop_during_init;
    assert(!startup.init() && !startup.is_ready() && !mock_gate);
    assert(mock_gate_rises == before);
    delay_hook = nullptr;
    assert(startup.init());

    // STOP during original driver's register initialization/readback rejects
    // even successful native init, including a pulse recovered before return.
    startup.physical_stop(true);
    mock_init_hook = stop_during_init;
    assert(!startup.init() && !startup.is_ready() && !mock_gate);
    mock_init_hook = nullptr;
    assert(startup.init());

    // Each delivered edge advances the fault generation; repeated low-level
    // fast guards stay idempotent. No calls to the native driver from ISR path.
    const auto epoch = startup.generation();
    const auto transfers = mock_inner_init_calls;
    startup.physical_stop(true); startup.physical_stop(true);
    assert(startup.generation() == epoch + 2);
    assert(mock_inner_init_calls == transfers);
    unsafe_motor = true;
    assert(!startup.init() && !mock_gate);
    unsafe_motor = false;
    mock_init_success = false;
    assert(!startup.init() && !mock_gate && !startup.is_ready());
    assert(!mock_critical_depth);

    // The cache invalidation sequence must work for every legal old gain,
    // including both endpoints used to force the configuration change.
    for (float requested : {10.0f, 20.0f, 40.0f, 80.0f}) {
        reset();
        V13GateDriver gain_case(nullptr, {}, {}, {}, stop_asserted, unsafe_to_init);
        active = &gain_case;
        assert(gain_case.config(requested, &gain) && gain_case.init());
        const unsigned writes_before = mock_register_writes;
        gain_case.physical_stop(true);
        assert(gain_case.init() && gain_case.is_ready());
        assert(mock_register_writes == writes_before + 1);
        assert(mock_gain_calls.back() == requested);
    }
    std::puts("v13_gate: startup, idle, cache restoration, STOP races and failures passed");
}
