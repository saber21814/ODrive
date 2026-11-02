#pragma once
#include <cassert>
extern unsigned mock_critical_depth;
extern bool mock_gate;
extern unsigned mock_gate_rises;
struct Stm32Gpio {
    void write(bool state) {
        assert(mock_critical_depth);
        if (state && !mock_gate) { ++mock_gate_rises; }
        mock_gate = state;
    }
};
