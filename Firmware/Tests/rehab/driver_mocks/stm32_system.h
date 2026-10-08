#pragma once
#include "spi.h"
extern uint32_t mock_primask, mock_ipsr;
inline uint32_t __get_IPSR() { return mock_ipsr; }
inline uint32_t __get_PRIMASK() { return mock_primask; }
inline void __DSB() {}
inline void __ISB() {}
extern uint32_t mock_cycle_step;
inline bool mock_dwt_running = true;
struct MockCounter { mutable uint32_t cycles = 0; operator uint32_t() const { if (mock_dwt_running) cycles += mock_cycle_step; return cycles; } };
struct MockDwt { uint32_t CTRL = 0; MockCounter CYCCNT; };
struct MockCoreDebug { uint32_t DEMCR = 0; };
struct MockNvic { uint32_t ISER[8]{}; uint32_t pending = 0; };
extern MockDwt mock_dwt;
extern MockCoreDebug mock_debug;
extern MockNvic mock_nvic;
#define DWT (&mock_dwt)
#define CoreDebug (&mock_debug)
#define NVIC (&mock_nvic)
#define CoreDebug_DEMCR_TRCENA_Msk 1u
#define DWT_CTRL_CYCCNTENA_Msk 1u
constexpr uint32_t SystemCoreClock = 168000000;
inline void __NOP() { if (mock_dwt_running) ++mock_dwt.CYCCNT.cycles; }
inline void NVIC_DisableIRQ(IRQn_Type irq) { mock_nvic.ISER[irq >> 5] &= ~(1u << (irq & 31)); }
inline void NVIC_EnableIRQ(IRQn_Type irq) { mock_nvic.ISER[irq >> 5] |= 1u << (irq & 31); }
inline void NVIC_ClearPendingIRQ(IRQn_Type irq) { mock_nvic.pending &= ~(1u << irq); }
struct CriticalSectionContext {
    inline static void (*before_enter)() = nullptr;
    uint32_t previous;
    CriticalSectionContext() : previous(mock_primask) {
        if(before_enter){const auto callback=before_enter;before_enter=nullptr;callback();}
        mock_primask = 1;
    }
    ~CriticalSectionContext() { mock_primask = previous; }
};
