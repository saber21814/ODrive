#pragma once
#include <cmath>
namespace rehab { namespace platform {
// Upper NTC to 3V3, lower 3.3k to ground. Invalid/near-rail readings fail closed.
inline float temperature_from_ratio(float ratio, float r25, float beta) {
    if (!std::isfinite(ratio) || ratio <= 0.001f || ratio >= 0.999f ||
        !std::isfinite(r25) || r25 <= 0.0f || !std::isfinite(beta) || beta <= 0.0f) return NAN;
    const float resistance = 3300.0f * (1.0f - ratio) / ratio;
    const float inv_kelvin = 1.0f / 298.15f + std::log(resistance / r25) / beta;
    return std::isfinite(inv_kelvin) && inv_kelvin > 0.0f ? 1.0f / inv_kelvin - 273.15f : NAN;
}
}}
