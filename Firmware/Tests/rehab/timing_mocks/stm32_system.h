#pragma once
#include <cstdint>

struct TimingDwt { volatile uint32_t CYCCNT = 0; };
extern TimingDwt timing_dwt;
#define DWT (&timing_dwt)
extern uint32_t timing_primask;
extern unsigned timing_critical_entries, timing_critical_depth;

struct CriticalSectionContext {
    CriticalSectionContext() : previous_(timing_primask) {
        timing_primask = 1;
        ++timing_critical_entries; ++timing_critical_depth;
    }
    ~CriticalSectionContext() { --timing_critical_depth; timing_primask = previous_; }
    CriticalSectionContext(const CriticalSectionContext&) = delete;
    CriticalSectionContext& operator=(const CriticalSectionContext&) = delete;
private:
    uint32_t previous_;
};
