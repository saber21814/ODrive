#pragma once
#include <cstdint>
namespace rehab {
struct ForceSample {
    uint32_t sequence = 0, timestamp_us = 0;
    int32_t raw = 0;
    bool valid = false;
};
}
