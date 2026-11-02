#pragma once
// Revision-specific hardware access. REHAB_V12 is the shared integration ABI;
// REHAB_V13 additionally selects the independent V1.3 board implementation.
#include "commissioning.hpp"
#include <stm32_spi_arbiter.hpp>
#include <stm32_gpio.hpp>

#if defined(REHAB_V13)
void rehab_board_physical_stop(bool edge);
bool rehab_board_gate_ready();
uint32_t rehab_board_gate_generation();
void rehab_board_service_spi(uint32_t now_ms);
Stm32SpiArbiter::Diagnostics rehab_board_native_spi_diagnostics();
#endif

namespace rehab { namespace platform { namespace board_access {
constexpr unsigned fet_temperature_channel = 15;
#if defined(REHAB_V13)
constexpr unsigned aux_temperature_channel = 8;
constexpr uint32_t revision = 0x1301;
inline Stm32Gpio stop_gpio() { return {STOP_GPIO_Port, STOP_Pin}; }
inline bool stop_asserted() { return !stop_gpio().read(); }
inline void physical_stop(bool edge) { rehab_board_physical_stop(edge); }
inline bool gate_ready() { return rehab_board_gate_ready(); }
inline uint32_t gate_generation() { return rehab_board_gate_generation(); }
inline void service_spi(uint32_t now_ms) { rehab_board_service_spi(now_ms); }
inline Stm32SpiArbiter::Diagnostics native_spi_diagnostics() { return rehab_board_native_spi_diagnostics(); }
#else
constexpr unsigned aux_temperature_channel = 5;
constexpr uint32_t revision = 0x1201;
inline Stm32Gpio stop_gpio() { return {GPIOA, GPIO_PIN_7}; }
inline bool stop_asserted() {
    return commissioning::stop_active_level >= 0 &&
        bool(GPIOA->IDR & GPIO_PIN_7) == bool(commissioning::stop_active_level);
}
inline void physical_stop(bool) {}
inline bool gate_ready() { return true; }
inline uint32_t gate_generation() { return 0; }
inline void service_spi(uint32_t now_ms) { ext_spi_arbiter.service(now_ms); }
inline auto native_spi_diagnostics() { return ext_spi_arbiter.diagnostics(); }
#endif
}}}
